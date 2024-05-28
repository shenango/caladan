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

pub struct UdpConnection(*mut ffi::udpconn_t);
impl UdpConnection {
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
        let ret = unsafe { ffi::udp_dial(laddr, raddr, &mut conn as *mut _) };
        if ret < 0 {
            Err(io::Error::from_raw_os_error(-ret as i32))
        } else {
            Ok(UdpConnection(conn))
        }
    }

    pub fn listen(local_addr: SocketAddrV4) -> io::Result<Self> {
        let laddr = ffi::netaddr {
            ip: NetworkEndian::read_u32(&local_addr.ip().octets()),
            port: local_addr.port(),
        };
        let mut conn = ptr::null_mut();
        let ret = unsafe { ffi::udp_listen(laddr, &mut conn as *mut _) };
        if ret < 0 {
            Err(io::Error::from_raw_os_error(-ret as i32))
        } else {
            Ok(UdpConnection(conn))
        }
    }

    pub fn set_buffers(&self, read_mbufs: u32, write_mbufs: u32) -> Result<(), i32> {
        convert_error(unsafe {
            ffi::udp_set_buffers(self.0, read_mbufs as c_int, write_mbufs as c_int)
        })
    }

    pub fn read_from(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddrV4)> {
        let mut raddr = ffi::netaddr { ip: 0, port: 0 };
        isize_to_result(unsafe {
            ffi::udp_read_from(
                self.0,
                buf.as_mut_ptr() as *mut c_void,
                buf.len() as _,
                &mut raddr as *mut _,
            )
        })
        .map(|u| (u, SocketAddrV4::new(raddr.ip.into(), raddr.port)))
    }

    pub fn write_to(&self, buf: &[u8], remote_addr: SocketAddrV4) -> io::Result<usize> {
        let mut raddr = ffi::netaddr {
            ip: NetworkEndian::read_u32(&remote_addr.ip().octets()),
            port: remote_addr.port(),
        };
        isize_to_result(unsafe {
            ffi::udp_write_to(
                self.0,
                buf.as_ptr() as *const c_void as *mut c_void,
                buf.len() as _,
                &mut raddr as *mut _,
            )
        })
    }

    /// Same as read, but doesn't take a &mut self.
    pub fn recv(&self, buf: &mut [u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::udp_read(self.0, buf.as_mut_ptr() as *mut c_void, buf.len() as _)
        })
    }
    /// Same as write, but doesn't take a &mut self.
    pub fn send(&self, buf: &[u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::udp_write(self.0, buf.as_ptr() as *const c_void, buf.len() as _)
        })
    }

    pub fn local_addr(&self) -> SocketAddrV4 {
        let local_addr = unsafe { ffi::udp_local_addr(self.0) };
        SocketAddrV4::new(local_addr.ip.into(), local_addr.port)
    }

    pub fn remote_addr(&self) -> SocketAddrV4 {
        let remote_addr = unsafe { ffi::udp_remote_addr(self.0) };
        SocketAddrV4::new(remote_addr.ip.into(), remote_addr.port)
    }

    pub fn shutdown(&self) {
        unsafe { ffi::udp_shutdown(self.0) }
    }
}

impl Read for UdpConnection {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::udp_read(self.0, buf.as_mut_ptr() as *mut c_void, buf.len() as _)
        })
    }
}

impl<'a> Read for &'a UdpConnection {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::udp_read(self.0, buf.as_mut_ptr() as *mut c_void, buf.len() as _)
        })
    }
}

impl Write for UdpConnection {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::udp_write(self.0, buf.as_ptr() as *const c_void, buf.len() as _)
        })
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl<'a> Write for &'a UdpConnection {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        isize_to_result(unsafe {
            ffi::udp_write(self.0, buf.as_ptr() as *const c_void, buf.len() as _)
        })
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Drop for UdpConnection {
    fn drop(&mut self) {
        unsafe { ffi::udp_close(self.0) }
    }
}

unsafe impl Send for UdpConnection {}
unsafe impl Sync for UdpConnection {}

pub struct UdpSpawner(*mut ffi::udpspawner_t);
impl UdpSpawner {
    pub unsafe fn new(
        local_addr: SocketAddrV4,
        f: extern "C" fn(*mut ffi::udp_spawn_data),
    ) -> io::Result<Self> {
        println!("{:?}", local_addr);
        let laddr = ffi::netaddr {
            ip: NetworkEndian::read_u32(&local_addr.ip().octets()),
            port: local_addr.port(),
        };

        let mut spawner: *mut ffi::udpspawner_t = ptr::null_mut();
        let ret = ffi::udp_create_spawner(laddr, Some(f), &mut spawner as *mut *mut _);

        if ret < 0 {
            Err(io::Error::from_raw_os_error(-ret as i32))
        } else {
            Ok(UdpSpawner(spawner))
        }
    }

    pub unsafe fn reply(d: *mut ffi::udp_spawn_data, buf: &[u8]) -> io::Result<usize> {
        isize_to_result(ffi::udp_send(
            buf.as_ptr() as *const c_void,
            buf.len() as _,
            (*d).laddr,
            (*d).raddr,
        ))
    }

    pub unsafe fn release_data(d: *mut ffi::udp_spawn_data) {
        ffi::udp_spawn_data_release((*d).release_data)
    }
}
impl Drop for UdpSpawner {
    fn drop(&mut self) {
        println!("!!!!!!!!!!!!");
        unsafe { ffi::udp_destroy_spawner(self.0) }
    }
}
