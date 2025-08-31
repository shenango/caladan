use std::io::{self, Read, Write};
use std::net::{Ipv4Addr, SocketAddrV4};
use std::time::Duration;

use crate::AppConfig;
use crate::ScheduleResult;

use crate::Backend;
use crate::Connection;
use crate::BARRIER_PORT;

use serde_binary::binary_stream::Endian;

pub enum Group {
    Server(Vec<Connection>),
    Client(Connection, usize, usize),
    Singleton,
}

enum Command {
    Sync = 1,
    SendResult = 2,
}

impl Group {
    pub fn create_client(addr: Ipv4Addr, backend: Backend) -> io::Result<(Self, AppConfig)> {
        let mut connection = Self::new_connection(SocketAddrV4::new(addr, BARRIER_PORT), backend)?;

        // Read full one u64.
        let mut buf: [u8; 8] = [0; 8];
        connection.read_exact(&mut buf)?;
        let nbytes = u64::from_be_bytes(buf);

        // Read full nbytes from socket and create a string.
        let mut buf = vec![0; nbytes as usize];
        connection.read_exact(&mut buf)?;
        let str = String::from_utf8(buf).unwrap();

        let decoded: AppConfig = serde_json::from_str(&str).unwrap();
        Ok((
            Group::Client(connection, decoded.rank, decoded.world_size),
            decoded,
        ))
    }

    pub fn shutdown(&mut self) {
        match self {
            Group::Server(ref mut clients) => {
                for c in clients.iter_mut() {
                    c.shutdown();
                }
            }
            _ => (),
        }
    }

    pub fn create_server(config: AppConfig) -> io::Result<(Self, AppConfig)> {
        if config.world_size == 1 {
            return Ok((Group::Singleton, config));
        }

        let configs = config.split();
        let addr = SocketAddrV4::new(Ipv4Addr::new(0, 0, 0, 0), BARRIER_PORT);
        let listener = config.backend.create_tcp_listener(addr)?;
        let mut clients = Vec::new();
        for i in 1..config.world_size {
            let encoded = serde_json::to_string(&configs[i]).unwrap();
            let nbytes = encoded.len() as u64;
            let mut buf = [0; 8];
            buf[..8].copy_from_slice(&nbytes.to_be_bytes());

            let mut conn = listener.accept()?;
            conn.write_all(&buf)?;
            conn.write_all(encoded.as_bytes())?;
            clients.push(conn);
        }

        Ok((Group::Server(clients), configs[0].clone()))
    }

    pub fn rank_and_size(&self) -> (usize, usize) {
        match self {
            Group::Server(ref clients) => (0, clients.len() + 1),
            Group::Client(_, rank, size) => (*rank, *size),
            Group::Singleton => (0, 1),
        }
    }

    pub fn sync_results(
        &mut self,
        packets: &mut Vec<Vec<Option<ScheduleResult>>>,
    ) -> io::Result<()> {
        let mut cmd: [u8; 1] = [0; 1];
        let mut size: [u8; 8] = [0; 8];

        match *self {
            Group::Singleton => (),
            Group::Server(ref mut clients) => {
                for c in clients.iter_mut() {
                    c.read_exact(&mut cmd)?;
                    assert_eq!(cmd[0], Command::SendResult as u8);
                    c.read_exact(&mut size)?;
                    let size = u64::from_be_bytes(size);
                    let mut buf = vec![0; size as usize];
                    c.read_exact(&mut buf)?;
                    let other_packets: Vec<Vec<Option<ScheduleResult>>> =
                        serde_binary::from_slice(&buf, Endian::Little).unwrap();
                    packets.extend(other_packets);
                    c.write_all(&cmd)?;
                }
            }
            Group::Client(ref mut stream, _, _) => {
                cmd[0] = Command::SendResult as u8;
                let serialized = serde_binary::to_vec(&packets, Endian::Little).unwrap();
                let sz = serialized.len() as u64;
                size[..8].copy_from_slice(&sz.to_be_bytes());
                stream.write_all(&cmd)?;
                stream.write_all(&size)?;
                stream.write_all(&serialized)?;
                stream.read_exact(&mut cmd)?;
                assert_eq!(cmd[0], Command::SendResult as u8);
            }
        };

        Ok(())
    }

    pub fn barrier(&mut self) -> io::Result<()> {
        let mut buf: [u8; 1] = [0; 1];
        buf[0] = Command::Sync as u8;

        match *self {
            Group::Singleton => (),
            Group::Server(ref mut clients) => {
                for c in clients.iter_mut() {
                    c.read_exact(&mut buf)?;
                    assert_eq!(buf[0], Command::Sync as u8);
                }
                for c in clients {
                    c.write_all(&buf)?;
                }
            }
            Group::Client(ref mut stream, _, _) => {
                stream.write_all(&buf)?;
                stream.read_exact(&mut buf)?;
                assert_eq!(buf[0], Command::Sync as u8);
            }
        }
        Ok(())
    }

    fn new_connection(addr: SocketAddrV4, backend: Backend) -> io::Result<Connection> {
        for _ in 0..5 {
            match backend.create_tcp_connection(None, addr) {
                Ok(stream) => return Ok(stream),
                Err(_) => backend.sleep(Duration::from_millis(50)),
            }
        }
        Ok(backend.create_tcp_connection(None, addr)?)
    }
}
