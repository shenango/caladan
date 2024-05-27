use crate::Buffer;
use crate::Connection;
use crate::LoadgenProtocol;
use crate::Packet;
use crate::Transport;

use byteorder::{BigEndian, WriteBytesExt};
use dns_parser::{Header, Opcode, QueryClass, QueryType, ResponseCode};

use std::io;
use std::io::{Error, ErrorKind, Read};

#[derive(Copy, Clone, Debug)]
pub struct DnsProtocol;

const NDOMAINS: usize = 100000;

#[inline(always)]
fn push_usize(mut i: usize, buf: &mut Vec<u8>) -> u8 {
    let mut pushed = 0;
    loop {
        buf.push(48 + (i % 10) as u8);
        i /= 10;
        pushed += 1;
        if i == 0 {
            break;
        }
    }
    pushed
}

#[inline(always)]
fn pull_usize(buf: &[u8]) -> usize {
    buf.iter()
        .enumerate()
        .map(|(idx, val)| ((*val as usize) - 48) * 10_usize.pow(idx as u32))
        .sum()
}

impl DnsProtocol {
    pub fn with_args(_matches: &clap::ArgMatches, tport: Transport) -> Self {
        match tport {
            Transport::Udp => (),
            _ => assert!(false),
        }
        DnsProtocol {}
    }

    pub fn args<'a, 'b>() -> Vec<clap::Arg<'a, 'b>> {
        vec![]
    }
}

impl LoadgenProtocol for DnsProtocol {
    fn uses_ordered_requests(&self) -> bool {
        false
    }

    fn gen_req(&self, i: usize, p: &Packet, buf: &mut Vec<u8>) {
        let h = Header {
            id: i as u16,
            query: true,
            opcode: Opcode::StandardQuery,
            authoritative: false,
            truncated: false,
            recursion_desired: false,
            recursion_available: false,
            authenticated_data: false,
            checking_disabled: false,
            response_code: ResponseCode::NoError,
            questions: 1,
            answers: 0,
            nameservers: 0,
            additional: 0,
        };

        buf.extend(vec![0; 12]);

        h.write(&mut buf[..12]);

        let size_idx = buf.len();
        buf.push(0);
        buf[size_idx] = push_usize(i, buf);

        let size_idx = buf.len();
        buf.push(0);
        buf[size_idx] = push_usize((p.randomness as usize) % NDOMAINS, buf);

        buf.push(3);
        buf.extend("com".as_bytes());

        buf.push(0);
        buf.write_u16::<BigEndian>(QueryType::A as u16).unwrap();
        buf.write_u16::<BigEndian>(QueryClass::IN as u16 | 0x8000)
            .unwrap();
    }

    fn read_response(&self, mut sock: &Connection, buf: &mut Buffer) -> io::Result<(usize, u64)> {
        let scratch = buf.get_empty_buf();
        let len = sock.read(&mut scratch[..])?;
        if len == 0 {
            return Err(Error::new(ErrorKind::UnexpectedEof, "eof"));
        }

        if Header::parse(scratch).is_err() || scratch[Header::size()] & 0b1100_0000 != 0 {
            return Err(Error::new(ErrorKind::Other, "bad packet!"));
        }

        let pos = Header::size();
        let end = pos + scratch[pos] as usize + 1;
        Ok((pull_usize(&scratch[pos + 1..end]), 0))
    }
}
