use clap::Arg;

use std::cmp::min;
use std::io;
use std::io::{Error, ErrorKind, Read};
use std::net::SocketAddrV4;
use std::str::FromStr;

use crate::Buffer;
use crate::Connection;
use crate::LoadgenProtocol;
use crate::Packet;
use crate::Transport;

#[derive(Copy, Clone)]
pub struct HttpProtocol {
    host: &'static str,
    uri: &'static str,
}

impl HttpProtocol {
    pub fn with_args(matches: &clap::ArgMatches, tport: Transport) -> Self {
        if let Transport::Udp = tport {
            panic!("udp is unsupported for http");
        }

        let host = match matches.value_of("http_host_name").unwrap() {
            "" => {
                let addr: SocketAddrV4 =
                    FromStr::from_str(matches.value_of("ADDR").unwrap()).unwrap();
                addr.ip().to_string()
            }
            s => s.to_string(),
        };

        HttpProtocol {
            host: Box::leak(host.into_boxed_str()),
            uri: Box::leak(
                matches
                    .value_of("http_uri")
                    .unwrap()
                    .to_string()
                    .into_boxed_str(),
            ),
        }
    }

    pub fn args<'a, 'b>() -> Vec<clap::Arg<'a, 'b>> {
        vec![
            Arg::with_name("http_host_name")
                .long("http_host_name")
                .takes_value(true)
                .default_value("")
                .help("Hostname to use in request"),
            Arg::with_name("http_uri")
                .long("http_uri")
                .takes_value(true)
                .default_value("/")
                .help("HTTP endpoint"),
        ]
    }
}

pub enum ParseState {
    Done(usize, usize), // Header len, body len
    Partial(usize, Option<usize>),
}

impl ParseState {
    #[inline(always)]

    pub fn new() -> ParseState {
        ParseState::Partial(0, None)
    }

    pub fn unwrap_partial(self) -> (usize, Option<usize>) {
        match self {
            ParseState::Partial(n, b) => (n, b),
            _ => panic!("Tried to unwrap_partial a complete parse state"),
        }
    }
}

#[inline(always)]
fn find_subsequence(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

fn header_extract(state: ParseState, buf: &[u8]) -> io::Result<ParseState> {
    if let ParseState::Done(_, _) = state {
        return Ok(state);
    }

    let (mut next_line_begin, mut content_len) = state.unwrap_partial();

    if next_line_begin == buf.len() {
        return Ok(ParseState::Partial(next_line_begin, content_len));
    }

    loop {
        match find_subsequence(&buf[next_line_begin..], b"\r\n") {
            Some(idx) => {
                if idx == 0 {
                    if content_len.is_none() {
                        return Err(Error::new(ErrorKind::Other, "missing content-length"));
                    }
                    return Ok(ParseState::Done(next_line_begin + 2, content_len.unwrap()));
                }
                let header_ln = &buf[next_line_begin..idx + next_line_begin];

                let utfln = unsafe { std::str::from_utf8_unchecked(&header_ln[..]) };
                if next_line_begin == 0 {
                    // Check for "HTTP/1.* 200*"
                    if !utfln[..7].eq_ignore_ascii_case("HTTP/1.")
                        || !utfln[8..12].eq_ignore_ascii_case(" 200")
                    {
                        return Err(Error::new(
                            ErrorKind::Other,
                            format!("got bad HTTP response: {}", utfln),
                        ));
                    }
                } else if utfln.eq_ignore_ascii_case("connection: close") {
                    return Err(Error::new(ErrorKind::Other, "server closed connection"));
                } else if header_ln.len() > 16
                    && utfln[..16].eq_ignore_ascii_case("content-length: ")
                {
                    let value = utfln[16..].parse::<usize>();
                    if value.is_err() {
                        return Err(Error::new(ErrorKind::Other, "content-length not u64"));
                    }
                    content_len = Some(value.unwrap());
                }

                next_line_begin += idx + 2;
            }
            None => {
                return Ok(ParseState::Partial(next_line_begin, content_len));
            }
        }
    }
}

impl LoadgenProtocol for HttpProtocol {
    fn uses_ordered_requests(&self) -> bool {
        true
    }

    fn gen_req(&self, _i: usize, _p: &Packet, buf: &mut Vec<u8>) {
        // GET {uri} HTTP/1.1\r\n
        // Connection: keep-alive\r\n
        // Host: {host}\r\n
        // \r\n

        buf.extend("GET ".as_bytes());
        buf.extend(self.uri.as_bytes());
        buf.extend(" HTTP/1.1\r\nConnection: keep-alive\r\nHost: ".as_bytes());
        buf.extend(self.host.as_bytes());
        buf.extend("\r\n\r\n".as_bytes());
    }

    fn read_response(&self, mut sock: &Connection, buf: &mut Buffer) -> io::Result<(usize, u64)> {
        let mut pstate = ParseState::new();

        if buf.data_size() == 0 {
            buf.try_shrink()?;
            let new_bytes = sock.read(buf.get_empty_buf())?;
            if new_bytes == 0 {
                return Err(Error::new(ErrorKind::UnexpectedEof, "eof"));
            }
            buf.push_data(new_bytes);
        }

        loop {
            let header_len;
            let body_len;

            match header_extract(pstate, buf.get_data())? {
                ParseState::Done(hlen, blen) => {
                    header_len = hlen;
                    body_len = blen;
                }

                ParseState::Partial(a, b) => {
                    pstate = ParseState::Partial(a, b);
                    buf.try_shrink()?;
                    let new_bytes = sock.read(buf.get_empty_buf())?;
                    if new_bytes == 0 {
                        return Err(Error::new(ErrorKind::UnexpectedEof, "eof"));
                    }
                    buf.push_data(new_bytes);
                    continue;
                }
            };

            /* drain socket if needed */
            let total_req_bytes = header_len + body_len;
            let curbytes = buf.data_size();
            buf.pull_data(min(curbytes, total_req_bytes));
            buf.try_shrink()?;
            if curbytes < total_req_bytes {
                let mut to_read = total_req_bytes - curbytes;
                while to_read > 0 {
                    let rlen = min(to_read, buf.get_free_space());
                    sock.read_exact(&mut buf.get_empty_buf()[..rlen])?;
                    to_read -= rlen;
                }
            }

            return Ok((0, 0));
        }
    }
}
