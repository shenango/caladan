use crate::Buffer;
use crate::Connection;
use crate::Distribution;
use crate::LoadgenProtocol;
use crate::Packet;
use crate::Transport;

use byteorder::{LittleEndian, ReadBytesExt, WriteBytesExt};
use clap::Arg;
use std::io;
use std::io::{Error, ErrorKind, Read};

use rand::distributions::Uniform;
use rand::Rng;
use rand_distr::Distribution as DistR;
use rand_mt::Mt64;

use std::cmp::min;

const REFLEX_HDR_SZ: usize = 32;
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
    tsc: u64,
}

impl PacketHeader {
    fn write<W: io::Write>(self, writer: &mut W) -> io::Result<()> {
        writer.write_u16::<LittleEndian>(REFLEX_MAGIC)?;
        writer.write_u16::<LittleEndian>(self.opcode)?;
        writer.write_u64::<LittleEndian>(self.req_handle as u64)?;
        writer.write_u64::<LittleEndian>(self.lba)?;
        writer.write_u32::<LittleEndian>(self.lba_count as u32)?;
        writer.write_u64::<LittleEndian>(self.tsc)?;
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
            tsc: reader.read_u64::<LittleEndian>()?,
        };
        return Ok(header);
    }
}

#[derive(Copy, Clone)]
pub struct ReflexProtocol {
    sectors_per_rq: Distribution,
    pct_set: u64,
}

static PAGE_DATA: &'static [u8] = &[0xab; 8 * SECTOR_SIZE];

impl ReflexProtocol {
    pub fn with_args(matches: &clap::ArgMatches, tport: Transport, dist: Distribution) -> Self {
        if let Transport::Udp = tport {
            panic!("udp is unsupported by the reflex protocol");
        }
        ReflexProtocol {
            sectors_per_rq: dist,
            pct_set: value_t!(matches, "reflex_set_rate", u64).unwrap(),
        }
    }

    pub fn set_request(&self, lba: u64, lba_count: usize, handle: usize, buf: &mut Vec<u8>) {
        PacketHeader {
            opcode: Opcode::Set as u16,
            req_handle: handle + 1,
            lba: lba,
            lba_count: lba_count,
            ..Default::default()
        }
        .write(buf)
        .unwrap();

        let mut to_send = lba_count * SECTOR_SIZE;
        while to_send > 0 {
            let rlen = min(to_send, PAGE_DATA.len());
            buf.extend_from_slice(&PAGE_DATA[..rlen]);
            to_send -= rlen;
        }
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
    fn uses_ordered_requests(&self) -> bool {
        false
    }

    fn gen_req(&self, i: usize, p: &Packet, buf: &mut Vec<u8>) {
        let mut rng: Mt64 = Mt64::new(p.randomness);
        let lba = (rng.gen::<u64>() % NUM_SECTORS) & LBA_ALIGNMENT;
        let mut lbacount = self.sectors_per_rq.sample(&mut rng);

        if lba + lbacount > NUM_SECTORS {
            lbacount = NUM_SECTORS - lba;
        }

        if Uniform::new(0, 1000).sample(&mut rng) < self.pct_set {
            self.set_request(lba, lbacount as usize, i, buf);
            return;
        }

        PacketHeader {
            opcode: Opcode::Get as u16,
            req_handle: i + 1,
            lba: lba,
            lba_count: lbacount as usize,
            ..Default::default()
        }
        .write(buf)
        .unwrap();
    }

    fn read_response(&self, mut sock: &Connection, buf: &mut Buffer) -> io::Result<(usize, u64)> {
        let scratch = buf.get_empty_buf();
        sock.read_exact(&mut scratch[..REFLEX_HDR_SZ])?;
        let hdr = PacketHeader::read(&mut &scratch[..])?;
        if hdr.opcode == Opcode::Get as u16 {
            let mut to_read = hdr.lba_count as usize;
            while to_read > 0 {
                let rlen = min(to_read, scratch.len());
                sock.read_exact(&mut scratch[..rlen])?;
                to_read -= rlen;
            }
        }

        // TODO: more error checking?

        Ok((hdr.req_handle - 1, hdr.tsc))
    }
}
