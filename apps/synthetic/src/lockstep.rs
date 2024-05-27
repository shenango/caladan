extern crate hostname;

use std::io::{self, Read, Write};
use std::net::SocketAddrV4;
use std::time::Duration;

use crate::Backend;
use crate::Connection;

pub enum Group {
    Server(Vec<Connection>),
    Client(Connection),
}

impl Group {
    pub fn new_server(
        num_clients: usize,
        addr: SocketAddrV4,
        backend: Backend,
    ) -> io::Result<Self> {
        let listener = backend.create_tcp_listener(addr)?;
        let mut clients = Vec::new();
        for _ in 0..num_clients {
            clients.push(listener.accept()?);
        }

        Ok(Group::Server(clients))
    }

    pub fn new_client(addr: SocketAddrV4, backend: Backend) -> io::Result<Self> {
        for _ in 0..5 {
            match backend.create_tcp_connection(None, addr) {
                Ok(stream) => return Ok(Group::Client(stream)),
                Err(_) => backend.sleep(Duration::from_millis(50)),
            }
        }
        Ok(Group::Client(backend.create_tcp_connection(None, addr)?))
    }

    pub fn barrier(&mut self) {
        let mut buf = [0; 1];

        match *self {
            Group::Server(ref mut clients) => {
                for c in clients.iter_mut() {
                    c.read_exact(&mut buf).unwrap();
                }
                buf[0] = 0;
                for c in clients {
                    c.write_all(&buf).unwrap();
                }
            }
            Group::Client(ref mut stream) => {
                stream.write_all(&buf).unwrap();
                stream.read_exact(&mut buf).unwrap();
            }
        }
    }
}
