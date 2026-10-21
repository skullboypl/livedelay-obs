//! AMF0: the encoding RTMP uses for commands and metadata.

#[derive(Debug, Clone, PartialEq)]
pub enum Amf {
    Number(f64),
    Bool(bool),
    Str(String),
    Object(Vec<(String, Amf)>),
    Null,
    Undefined,
    EcmaArray(Vec<(String, Amf)>),
    StrictArray(Vec<Amf>),
}

impl Amf {
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Amf::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_num(&self) -> Option<f64> {
        match self {
            Amf::Number(n) => Some(*n),
            _ => None,
        }
    }

    pub fn get(&self, key: &str) -> Option<&Amf> {
        match self {
            Amf::Object(p) | Amf::EcmaArray(p) => p.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
}

pub fn obj(props: &[(&str, Amf)]) -> Amf {
    Amf::Object(props.iter().map(|(k, v)| (k.to_string(), v.clone())).collect())
}

pub fn s(v: &str) -> Amf {
    Amf::Str(v.to_string())
}

fn put_key(out: &mut Vec<u8>, k: &str) {
    out.extend_from_slice(&(k.len() as u16).to_be_bytes());
    out.extend_from_slice(k.as_bytes());
}

fn put_props(out: &mut Vec<u8>, props: &[(String, Amf)]) {
    for (k, v) in props {
        put_key(out, k);
        encode(v, out);
    }
    out.extend_from_slice(&[0, 0, 9]);
}

pub fn encode(v: &Amf, out: &mut Vec<u8>) {
    match v {
        Amf::Number(n) => {
            out.push(0);
            out.extend_from_slice(&n.to_be_bytes());
        }
        Amf::Bool(b) => out.extend_from_slice(&[1, *b as u8]),
        Amf::Str(st) if st.len() > 0xFFFF => {
            out.push(0x0c);
            out.extend_from_slice(&(st.len() as u32).to_be_bytes());
            out.extend_from_slice(st.as_bytes());
        }
        Amf::Str(st) => {
            out.push(2);
            put_key(out, st);
        }
        Amf::Object(p) => {
            out.push(3);
            put_props(out, p);
        }
        Amf::Null => out.push(5),
        Amf::Undefined => out.push(6),
        Amf::EcmaArray(p) => {
            out.push(8);
            out.extend_from_slice(&(p.len() as u32).to_be_bytes());
            put_props(out, p);
        }
        Amf::StrictArray(items) => {
            out.push(0x0a);
            out.extend_from_slice(&(items.len() as u32).to_be_bytes());
            for i in items {
                encode(i, out);
            }
        }
    }
}

pub fn encode_all(values: &[Amf]) -> Vec<u8> {
    let mut out = Vec::new();
    for v in values {
        encode(v, &mut out);
    }
    out
}

fn take<'a>(b: &mut &'a [u8], n: usize) -> Option<&'a [u8]> {
    if b.len() < n {
        return None;
    }
    let (head, rest) = b.split_at(n);
    *b = rest;
    Some(head)
}

fn read_u16(b: &mut &[u8]) -> Option<usize> {
    take(b, 2).map(|x| u16::from_be_bytes([x[0], x[1]]) as usize)
}

fn read_u32(b: &mut &[u8]) -> Option<usize> {
    take(b, 4).map(|x| u32::from_be_bytes([x[0], x[1], x[2], x[3]]) as usize)
}

fn read_utf8(b: &mut &[u8], n: usize) -> Option<String> {
    take(b, n).map(|x| String::from_utf8_lossy(x).into_owned())
}

fn read_props(b: &mut &[u8], depth: u8) -> Option<Vec<(String, Amf)>> {
    let mut props = Vec::new();
    loop {
        let n = read_u16(b)?;
        if n == 0 {
            take(b, 1)?; // object-end marker (9)
            return Some(props);
        }
        let k = read_utf8(b, n)?;
        props.push((k, decode_one(b, depth + 1)?));
    }
}

fn decode_one(b: &mut &[u8], depth: u8) -> Option<Amf> {
    if depth > 16 {
        return None;
    }
    let marker = take(b, 1)?[0];
    Some(match marker {
        0 => {
            let x = take(b, 8)?;
            Amf::Number(f64::from_be_bytes(x.try_into().ok()?))
        }
        1 => Amf::Bool(take(b, 1)?[0] != 0),
        2 => {
            let n = read_u16(b)?;
            Amf::Str(read_utf8(b, n)?)
        }
        3 => Amf::Object(read_props(b, depth)?),
        5 => Amf::Null,
        6 => Amf::Undefined,
        8 => {
            read_u32(b)?;
            Amf::EcmaArray(read_props(b, depth)?)
        }
        0x0a => {
            let n = read_u32(b)?;
            let mut items = Vec::new();
            for _ in 0..n {
                items.push(decode_one(b, depth + 1)?);
            }
            Amf::StrictArray(items)
        }
        0x0b => {
            let x = take(b, 10)?;
            Amf::Number(f64::from_be_bytes(x[..8].try_into().ok()?))
        }
        0x0c => {
            let n = read_u32(b)?;
            Amf::Str(read_utf8(b, n)?)
        }
        _ => return None,
    })
}

/// Decodes values until the input ends or something unreadable appears.
pub fn decode_all(mut b: &[u8]) -> Vec<Amf> {
    let mut out = Vec::new();
    while !b.is_empty() {
        match decode_one(&mut b, 0) {
            Some(v) => out.push(v),
            None => break,
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let v = vec![
            s("connect"),
            Amf::Number(1.0),
            obj(&[("app", s("live")), ("ok", Amf::Bool(true))]),
            Amf::Null,
            Amf::EcmaArray(vec![("width".into(), Amf::Number(1920.0))]),
        ];
        assert_eq!(decode_all(&encode_all(&v)), v);
    }
}
