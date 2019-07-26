use byteorder::{LittleEndian, ReadBytesExt, WriteBytesExt};
use std::io;
use std::io::{Error, ErrorKind, Read};

use Connection;
use Packet;
use Transport;

const REFLEX_HDR_SZ: usize = 24;
const REFLEX_MAGIC: u16 = REFLEX_HDR_SZ as u16;

// FIXME - these may be specific to our device
const NUM_SECTORS: u64 = 547002288;
const SECTORS_PER_REQ: usize = 8;
const LBA_ALIGNMENT: u64 = !0x7;
const SECTOR_SIZE: usize = 512;

const PCT_SET: u64 = 0; // out of 1000

#[allow(dead_code)]
enum Opcode {
    Get = 0x00,
    Set = 0x01,
    SetNoAck = 0x02,
}

#[allow(dead_code)]
enum Response {
    NoError = 0x00,
    InvalidArguments = 0x04,
}

#[derive(Default)]
struct PacketHeader {
    req_handle: usize,
    lba: u64,
    lba_count: usize,
    opcode: u16,
}

impl PacketHeader {
    fn write<W: io::Write>(self, writer: &mut W) -> io::Result<()> {
        writer.write_u16::<LittleEndian>(REFLEX_MAGIC)?;
        writer.write_u16::<LittleEndian>(self.opcode)?;
        writer.write_u64::<LittleEndian>(self.req_handle as u64)?;
        writer.write_u64::<LittleEndian>(self.lba)?;
        writer.write_u32::<LittleEndian>(self.lba_count as u32)?;
        return Ok(());
    }

    fn read<R: io::Read>(reader: &mut R) -> io::Result<PacketHeader> {
        let magic = reader.read_u16::<LittleEndian>()?;
        if magic != REFLEX_MAGIC {
            return Err(Error::new(
                ErrorKind::Other,
                format!("Bad magic number in response header: {}", magic),
            ));
        }
        let header = PacketHeader {
            opcode: reader.read_u16::<LittleEndian>()?,
            req_handle: reader.read_u64::<LittleEndian>()? as usize,
            lba: reader.read_u64::<LittleEndian>()?,
            lba_count: reader.read_u32::<LittleEndian>()? as usize,
        };
        return Ok(header);
    }
}

#[derive(Copy, Clone, Debug)]
pub struct ReflexProtocol;

static PAGE_DATA: &'static [u8] = &[0xab; SECTORS_PER_REQ * SECTOR_SIZE];

impl ReflexProtocol {
    pub fn set_request(lba: u64, handle: usize, buf: &mut Vec<u8>) {
        PacketHeader {
            opcode: Opcode::Set as u16,
            req_handle: handle,
            lba: lba,
            lba_count: SECTORS_PER_REQ,
            ..Default::default()
        }
        .write(buf)
        .unwrap();

        buf.extend_from_slice(PAGE_DATA);
    }

    pub fn gen_request(i: usize, p: &Packet, buf: &mut Vec<u8>, tport: Transport) {
        if let Transport::Udp = tport {
            panic!("udp is unsupported by the reflex protocol");
        }

        // Use first 32 bits of randomness to determine if this is a SET or GET req
        let low32 = p.randomness & 0xffffffff;
        let lba = ((p.randomness >> 32) % NUM_SECTORS) & LBA_ALIGNMENT;

        if low32 % 1000 < PCT_SET {
            ReflexProtocol::set_request(lba, i, buf);
            return;
        }

        PacketHeader {
            opcode: Opcode::Get as u16,
            req_handle: i,
            lba: lba,
            lba_count: SECTORS_PER_REQ,
            ..Default::default()
        }
        .write(buf)
        .unwrap();
    }

    pub fn read_response(
        mut sock: &Connection,
        tport: Transport,
        scratch: &mut [u8],
    ) -> io::Result<usize> {
        if let Transport::Udp = tport {
            panic!("udp is unsupported by the reflex protocol");
        }

        sock.read_exact(&mut scratch[..REFLEX_HDR_SZ])?;
        let hdr = PacketHeader::read(&mut &scratch[..])?;
        if hdr.opcode == Opcode::Get as u16 {
            sock.read_exact(&mut scratch[..SECTOR_SIZE * hdr.lba_count])?;
        }

        // TODO: more error checking?

        Ok(hdr.req_handle)
    }
}
