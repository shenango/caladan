use libc;
use shenango;
use shenango::tcp::TcpConnection;
use shenango::udp::UdpConnection;
use std;

use std::any::Any;
use std::io;
use std::io::{Error, ErrorKind, Read, Write};
use std::net::{SocketAddr, SocketAddrV4, TcpListener, TcpStream, UdpSocket};
use std::os::unix::io::AsRawFd;
use std::thread;
use std::time::Duration;

use net2::unix::UnixUdpBuilderExt;
use net2::TcpBuilder;
use net2::UdpBuilder;

#[derive(Copy, Clone)]
pub enum Backend {
    Linux,
    Runtime,
}
impl Backend {
    pub fn create_udp_connection(
        &self,
        local_addr: SocketAddrV4,
        remote_addr: Option<SocketAddrV4>,
    ) -> io::Result<Connection> {
        Ok(match (self, remote_addr) {
            (&Backend::Linux, None) => Connection::LinuxUdp(
                UdpBuilder::new_v4()?
                    .reuse_address(true)?
                    .reuse_port(true)?
                    .bind(local_addr)?,
            ),
            (&Backend::Runtime, None) => Connection::RuntimeUdp(UdpConnection::listen(local_addr)?),
            (&Backend::Linux, Some(remote_addr)) => {
                let socket = UdpSocket::bind(local_addr)?;
                socket.connect(remote_addr)?;
                Connection::LinuxUdp(socket)
            }
            (&Backend::Runtime, Some(remote_addr)) => {
                Connection::RuntimeUdp(UdpConnection::dial(local_addr, remote_addr)?)
            }
        })
    }

    pub fn create_tcp_connection(
        &self,
        local_addr: Option<SocketAddrV4>,
        remote_addr: SocketAddrV4,
    ) -> io::Result<Connection> {
        let laddr = match local_addr {
            Some(x) => x,
            _ => "0.0.0.0:0".parse().unwrap(),
        };
        Ok(match *self {
            Backend::Linux => Connection::LinuxTcp(TcpStream::connect(remote_addr)?),
            Backend::Runtime => Connection::RuntimeTcp(TcpConnection::dial(laddr, remote_addr)?),
        })
    }

    pub fn create_tcp_listener(&self, local_addr: SocketAddrV4) -> io::Result<ConnectionListener> {
        Ok(match *self {
            Backend::Linux => {
                ConnectionListener::LinuxTcp(TcpBuilder::new_v4()?.bind(local_addr)?.listen(1024)?)
            }
            Backend::Runtime => {
                ConnectionListener::RuntimeTcp(shenango::tcp::TcpQueue::listen(local_addr, 1024)?)
            }
        })
    }

    pub fn spawn_thread<T, F>(&self, f: F) -> JoinHandle<T>
    where
        T: Send,
        F: FnOnce() -> T,
        F: Send + 'static,
    {
        match *self {
            Backend::Linux => JoinHandle::Linux(thread::spawn(f)),
            Backend::Runtime => JoinHandle::Runtime(shenango::thread::spawn(f)),
        }
    }

    pub fn sleep(&self, duration: Duration) {
        match *self {
            Backend::Linux => thread::sleep(duration),
            Backend::Runtime => shenango::sleep(duration),
        }
    }

    #[allow(unused)]
    pub fn thread_yield(&self) {
        match *self {
            Backend::Linux => thread::yield_now(),
            Backend::Runtime => shenango::thread::thread_yield(),
        }
    }

    pub fn init_and_run<'a, F>(&self, cfgpath: Option<&'a str>, f: F)
    where
        F: FnOnce(),
        F: Send + 'static,
    {
        match *self {
            Backend::Linux => f(),
            Backend::Runtime => shenango::runtime_init(cfgpath.unwrap().to_owned(), f).unwrap(),
        }
    }
}

pub enum ConnectionListener {
    LinuxTcp(TcpListener),
    RuntimeTcp(shenango::tcp::TcpQueue),
}

impl ConnectionListener {
    pub fn accept(&self) -> io::Result<Connection> {
        match *self {
            ConnectionListener::RuntimeTcp(ref s) => Ok(Connection::RuntimeTcp(s.accept()?)),
            ConnectionListener::LinuxTcp(ref s) => {
                let (socket, _addr) = s.accept()?;
                socket.set_nodelay(true)?;
                Ok(Connection::LinuxTcp(socket))
            }
        }
    }
    #[allow(unused)]
    pub fn shutdown(&self) {
        match *self {
            ConnectionListener::RuntimeTcp(ref s) => s.shutdown(),
            ConnectionListener::LinuxTcp(ref s) => unsafe {
                let _ = libc::shutdown(s.as_raw_fd(), libc::SHUT_RDWR);
            },
        }
    }
}

pub enum Connection {
    LinuxTcp(TcpStream),
    LinuxUdp(UdpSocket),
    RuntimeUdp(shenango::udp::UdpConnection),
    RuntimeTcp(shenango::tcp::TcpConnection),
}

impl Connection {
    pub fn send_to(&self, buf: &[u8], addr: SocketAddrV4) -> io::Result<usize> {
        match *self {
            Connection::LinuxUdp(ref s) => s.send_to(buf, addr),
            Connection::RuntimeUdp(ref s) => s.write_to(buf, addr),
            _ => Err(Error::new(ErrorKind::Other, "unimplemented")),
        }
    }
    pub fn recv_from(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddrV4)> {
        match *self {
            Connection::LinuxUdp(ref s) => s.recv_from(buf).map(|(len, addr)| match addr {
                SocketAddr::V4(addr) => (len, addr),
                _ => unreachable!(),
            }),
            Connection::RuntimeUdp(ref s) => s.read_from(buf),
            _ => Err(Error::new(ErrorKind::Other, "unimplemented")),
        }
    }

    pub fn local_addr(&self) -> SocketAddrV4 {
        match *self {
            Connection::LinuxUdp(ref s) => match s.local_addr() {
                Ok(SocketAddr::V4(addr)) => addr,
                _ => unreachable!(),
            },
            Connection::LinuxTcp(ref s) => match s.local_addr() {
                Ok(SocketAddr::V4(addr)) => addr,
                _ => unreachable!(),
            },
            Connection::RuntimeUdp(ref s) => s.local_addr(),
            Connection::RuntimeTcp(ref s) => s.local_addr(),
        }
    }

    #[allow(unused)]
    pub fn shutdown(&self) {
        match *self {
            Connection::LinuxUdp(ref s) => unsafe {
                let _ = libc::shutdown(s.as_raw_fd(), libc::SHUT_RDWR);
            },
            Connection::LinuxTcp(ref s) => unsafe {
                let _ = libc::shutdown(s.as_raw_fd(), libc::SHUT_RDWR);
            },
            Connection::RuntimeUdp(ref s) => s.shutdown(),
            Connection::RuntimeTcp(ref s) => {
                if s.shutdown(libc::SHUT_RDWR).is_err() {
                    s.abort()
                }
            }
        }
    }
}

impl Read for Connection {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match *self {
            Connection::LinuxUdp(ref s) => s.recv(buf),
            Connection::LinuxTcp(ref mut s) => s.read(buf),
            Connection::RuntimeUdp(ref mut s) => s.read(buf),
            Connection::RuntimeTcp(ref mut s) => s.read(buf),
        }
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        if let Connection::RuntimeTcp(ref mut s) = *self {
            s.abort();
        }
    }
}

impl<'a> Read for &'a Connection {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match *self {
            Connection::LinuxUdp(ref s) => s.recv(buf),
            Connection::LinuxTcp(ref s) => (&*s).read(buf),
            Connection::RuntimeUdp(ref s) => (&*s).read(buf),
            Connection::RuntimeTcp(ref s) => (&*s).read(buf),
        }
    }
}

impl<'a> Write for &'a Connection {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match *self {
            Connection::LinuxUdp(ref s) => s.send(buf),
            Connection::LinuxTcp(ref s) => (&*s).write(buf),
            Connection::RuntimeUdp(ref s) => (&*s).write(buf),
            Connection::RuntimeTcp(ref s) => (&*s).write(buf),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match *self {
            Connection::LinuxUdp(_) => Ok(()),
            Connection::LinuxTcp(ref s) => (&*s).flush(),
            Connection::RuntimeUdp(ref s) => (&*s).flush(),
            Connection::RuntimeTcp(ref s) => (&*s).flush(),
        }
    }
}

impl Write for Connection {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match *self {
            Connection::LinuxUdp(ref s) => s.send(buf),
            Connection::LinuxTcp(ref mut s) => s.write(buf),
            Connection::RuntimeUdp(ref mut s) => s.write(buf),
            Connection::RuntimeTcp(ref mut s) => s.write(buf),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match *self {
            Connection::LinuxUdp(_) => Ok(()),
            Connection::LinuxTcp(ref mut s) => s.flush(),
            Connection::RuntimeUdp(ref mut s) => s.flush(),
            Connection::RuntimeTcp(ref mut s) => s.flush(),
        }
    }
}

pub enum JoinHandle<T: Send + 'static> {
    Linux(std::thread::JoinHandle<T>),
    Runtime(shenango::thread::JoinHandle<T>),
}
impl<T: Send + 'static> JoinHandle<T> {
    pub fn join(self) -> Result<T, Box<dyn Any + Send + 'static>> {
        match self {
            JoinHandle::Linux(j) => j.join(),
            JoinHandle::Runtime(j) => j.join(),
        }
    }
}
