use byteorder::{LittleEndian, ReadBytesExt, WriteBytesExt};
use clap::Arg;
use std::io;
use std::io::{Error, ErrorKind, Read};

use Connection;
use LoadgenProtocol;
use Packet;
use Transport;

const REFLEX_HDR_SZ: usize = 24;
const REFLEX_MAGIC: u16 = REFLEX_HDR_SZ as u16;

// FIXME - these may be specific to our device
const NUM_SECTORS: u64 = 547002288;
const LBA_ALIGNMENT: u64 = !0x7;
const SECTOR_SIZE: usize = 512;

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

#[derive(Copy, Clone)]
pub struct ReflexProtocol {
    sectors_per_rq: usize,
    pct_set: u64,
}

static PAGE_DATA: &'static [u8] = &[0xab; 8 * SECTOR_SIZE];

impl ReflexProtocol {
    pub fn with_args(matches: &clap::ArgMatches, tport: Transport) -> Self {
        if let Transport::Udp = tport {
            panic!("udp is unsupported by the reflex protocol");
        }
        let sr = value_t!(matches, "request_size", usize).unwrap();
        if sr > 8 {
            panic!("please recompile with support for >8 sectors")
        }
        ReflexProtocol {
            sectors_per_rq: sr,
            pct_set: value_t!(matches, "reflex_set_rate", u64).unwrap(),
        }
    }

    pub fn set_request(&self, lba: u64, handle: usize, buf: &mut Vec<u8>) {
        PacketHeader {
            opcode: Opcode::Set as u16,
            req_handle: handle + 1,
            lba: lba,
            lba_count: self.sectors_per_rq,
            ..Default::default()
        }
        .write(buf)
        .unwrap();

        buf.extend_from_slice(&PAGE_DATA[..self.sectors_per_rq * SECTOR_SIZE]);
    }

    pub fn args<'a, 'b>() -> Vec<clap::Arg<'a, 'b>> {
        vec![
            Arg::with_name("reflex_set_rate")
                .long("reflex_set_rate")
                .takes_value(true)
                .default_value("0")
                .help("Reflex: set requsts per 1000 requests"),
            Arg::with_name("request_size")
                .long("request_size")
                .takes_value(true)
                .default_value("8")
                .help("Reflex: request size in sectors"),
        ]
    }
}

impl LoadgenProtocol for ReflexProtocol {
    fn gen_req(&self, i: usize, p: &Packet, buf: &mut Vec<u8>) {
        // Use first 32 bits of randomness to determine if this is a SET or GET req
        let low32 = p.randomness & 0xffffffff;
        let lba = ((p.randomness >> 32) % NUM_SECTORS) & LBA_ALIGNMENT;

        if low32 % 1000 < self.pct_set {
            self.set_request(lba, i, buf);
            return;
        }

        PacketHeader {
            opcode: Opcode::Get as u16,
            req_handle: i + 1,
            lba: lba,
            lba_count: self.sectors_per_rq,
            ..Default::default()
        }
        .write(buf)
        .unwrap();
    }

    fn read_response(&self, mut sock: &Connection, scratch: &mut [u8]) -> io::Result<usize> {
        sock.read_exact(&mut scratch[..REFLEX_HDR_SZ])?;
        let hdr = PacketHeader::read(&mut &scratch[..])?;
        if hdr.opcode == Opcode::Get as u16 {
            sock.read_exact(&mut scratch[..SECTOR_SIZE * hdr.lba_count])?;
        }

        // TODO: more error checking?

        Ok(hdr.req_handle - 1)
    }
}
