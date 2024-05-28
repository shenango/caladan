use std::io::{self, Read, Write};
use std::net::SocketAddrV4;
use std::ptr;

use byteorder::{ByteOrder, NetworkEndian};

use super::*;

fn isize_to_result(i: isize) -> io::Result<usize> {
    if i >= 0 {
        Ok(i as usize)
    } else {
        Err(io::Error::from_raw_os_error(-i as i32))
    }
}

pub struct TcpQueue(*mut ffi::tcpqueue_t);
impl TcpQueue {
    pub fn listen(local_addr: SocketAddrV4, backlog: i32) -> io::Result<Self> {
        let laddr = ffi::netaddr {
            ip: NetworkEndian::read_u32(&local_addr.ip().octets()),
            port: local_addr.port(),
        };
        let mut queue = ptr::null_mut();
        let ret = unsafe { ffi::tcp_listen(laddr, backlog, &mut queue as *mut _) };
        if ret < 0 {
            Err(io::Error::from_raw_os_error(-ret as i32))
        } else {
            Ok(TcpQueue(queue))
        }
    }
    pub fn accept(&self) -> io::Result<TcpConnection> {
        let mut conn = ptr::null_mut();
        let ret = unsafe { ffi::tcp_accept(self.0, &mut conn as *mut _) };
        if ret < 0 {
            Err(io::Error::from_raw_os_error(-ret as i32))
        } else {
            Ok(TcpConnection(conn))
        }
    }
    pub fn shutdown(&self) {
        unsafe { ffi::tcp_qshutdown(self.0) }
    }
}
impl Drop for TcpQueue {
    fn drop(&mut self) {
        unsafe { ffi::tcp_qclose(self.0) }
    }
}
unsafe impl Send for TcpQueue {}
unsafe impl Sync for TcpQueue {}

pub struct TcpConnection(*mut ffi::tcpconn_t);
impl TcpConnection {
    pub fn dial(local_addr: SocketAddrV4, remote_addr: SocketAddrV4) -> io::Result<Self> {
        let laddr = ffi::netaddr {
            ip: NetworkEndian::read_u32(&local_addr.ip().octets()),
            port: local_addr.port(),
        };
        let raddr = ffi::netaddr {
            ip: NetworkEndian::read_u32(&remote_addr.ip().octets()),
            port: remote_addr.port(),
        };

        let mut conn = ptr::null_mut();
        let ret = unsafe { ffi::tcp_dial(laddr, raddr, &mut conn as *mut _) };
        if ret < 0 {
            Err(io::Error::from_raw_os_error(-ret as i32))
        } else {
            Ok(TcpConnection(conn))
        }
    }

    pub fn local_addr(&self) -> SocketAddrV4 {
        let local_addr = unsafe { ffi::tcp_local_addr(self.0) };
        SocketAddrV4::new(local_addr.ip.into(), local_addr.port)
    }

    pub fn remote_addr(&self) -> SocketAddrV4 {
        let remote_addr = unsafe { ffi::tcp_remote_addr(self.0) };
        SocketAddrV4::new(remote_addr.ip.into(), remote_addr.port)
    }

    pub fn shutdown(&self, how: c_int) -> io::Result<()> {
        let res = unsafe { ffi::tcp_shutdown(self.0, how) };
        if res == 0 {
            Ok(())
        } else {
            Err(io::Error::from_raw_os_error(-res as i32))
        }
    }

    pub fn abort(&self) {
        unsafe { ffi::tcp_abort(self.0) };
    }
}

impl<'a> Read for &'a TcpConnection {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::tcp_read(self.0, buf.as_mut_ptr() as *mut c_void, buf.len() as _) as _
        })
    }
}
impl Read for TcpConnection {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::tcp_read(self.0, buf.as_mut_ptr() as *mut c_void, buf.len() as _) as _
        })
    }
}
impl<'a> Write for &'a TcpConnection {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::tcp_write(self.0, buf.as_ptr() as *const c_void, buf.len() as _) as _
        })
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
impl Write for TcpConnection {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::tcp_write(self.0, buf.as_ptr() as *const c_void, buf.len() as _)
        })
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
impl Drop for TcpConnection {
    fn drop(&mut self) {
        unsafe { ffi::tcp_close(self.0) }
    }
}
unsafe impl Send for TcpConnection {}
unsafe impl Sync for TcpConnection {}
