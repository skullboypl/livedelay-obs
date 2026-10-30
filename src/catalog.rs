//! OBS's own platform list (services.json + ingest caches) and the stream
//! target saved in the current OBS profile, so the panel can offer the same
//! servers and copy a server and key across.

use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone)]
struct Server {
    name: String,
    url: String,
}

struct Service {
    name: String,
    common: bool,
    key_link: String,
    max_video_kbps: i64,
    max_audio_kbps: i64,
    keyint_sec: i64,
    servers: Vec<Server>,
}

struct Profile {
    name: String,
    service: String,
    server: String,
    url: String,
    key: String,
}

pub fn services_json() -> String {
    let mut list = load_services();
    if !list.iter().any(|s| s.name == "Kick") {
        list.push(Service {
            name: "Kick".into(),
            common: true,
            key_link: "https://dashboard.kick.com/settings/stream".into(),
            max_video_kbps: 8000,
            max_audio_kbps: 320,
            keyint_sec: 2,
            // Every Kick channel has its own ingest host, so there is nothing
            // sensible to ship here: the user pastes theirs from the dashboard.
            servers: vec![Server { name: "Z panelu Kick".into(), url: String::new() }],
        });
    }
    list.sort_by(|a, b| b.common.cmp(&a.common).then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase())));
    let mut out = String::from("{\"services\":[");
    for (i, s) in list.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"name\":\"{}\",\"common\":{},\"key_link\":\"{}\",\"max_video_kbps\":{},\"max_audio_kbps\":{},\"keyint_sec\":{},\"servers\":[",
            esc(&s.name),
            s.common,
            esc(&s.key_link),
            s.max_video_kbps,
            s.max_audio_kbps,
            s.keyint_sec
        ));
        for (j, srv) in s.servers.iter().enumerate() {
            if j > 0 {
                out.push(',');
            }
            out.push_str(&format!("{{\"name\":\"{}\",\"url\":\"{}\"}}", esc(&srv.name), esc(&srv.url)));
        }
        out.push_str("]}");
    }
    out.push_str("]}");
    out
}

/// Stream targets saved in OBS profiles. Localhost only: each entry includes the key.
pub fn obs_import_json() -> String {
    let current = current_profile_dir();
    let services = load_services();
    let profiles = read_profiles(&services);
    let mut out = format!("{{\"current\":\"{}\",\"profiles\":[", esc(current.as_deref().unwrap_or("")));
    for (i, p) in profiles.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"name\":\"{}\",\"service\":\"{}\",\"server\":\"{}\",\"url\":\"{}\",\"has_key\":{},\"key\":\"{}\"}}",
            esc(&p.name),
            esc(&p.service),
            esc(&p.server),
            esc(&p.url),
            !p.key.is_empty(),
            esc(&p.key)
        ));
    }
    out.push_str("]}");
    out
}

fn load_services() -> Vec<Service> {
    let text = read_services_file();
    let mut list = text.as_deref().and_then(|t| parse_services(t)).unwrap_or_default();
    if let Some(dir) = rtmp_dir() {
        if let Some(ing) = read_ingests(&dir.join("twitch_ingests.json")) {
            replace_ingests(&mut list, "Twitch", &ing);
        }
        if let Some(ing) = read_ingests(&dir.join("amazon_ivs_ingests.json")) {
            replace_ingests(&mut list, "Amazon IVS", &ing);
        }
    }
    list
}

fn read_services_file() -> Option<String> {
    let mut best: Option<(u64, String)> = None;
    let mut consider = |path: PathBuf| {
        let meta = fs::metadata(&path).ok()?;
        let modified = meta.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_secs()).unwrap_or(0);
        if best.as_ref().map(|(t, _)| modified >= *t).unwrap_or(true) {
            if let Ok(text) = fs::read_to_string(&path) {
                if text.contains("\"services\"") {
                    best = Some((modified, text));
                }
            }
        }
        Some(())
    };
    if let Some(dir) = rtmp_dir() {
        consider(dir.join("services.json"));
    }
    if let Some(pf) = std::env::var_os("ProgramFiles") {
        consider(PathBuf::from(pf).join("obs-studio/data/obs-plugins/rtmp-services/services.json"));
    }
    consider(PathBuf::from(r"C:\Program Files\obs-studio\data\obs-plugins\rtmp-services\services.json"));
    best.map(|(_, text)| text)
}

fn rtmp_dir() -> Option<PathBuf> {
    std::env::var_os("APPDATA").map(|p| PathBuf::from(p).join("obs-studio/plugin_config/rtmp-services"))
}

fn obs_root() -> Option<PathBuf> {
    std::env::var_os("APPDATA").map(|p| PathBuf::from(p).join("obs-studio"))
}

struct Ingest {
    name: String,
    url: String,
    priority: i64,
}

fn read_ingests(path: &Path) -> Option<Vec<Ingest>> {
    let text = fs::read_to_string(path).ok()?;
    let v = parse_json(&text)?;
    let arr = obj_get(&v, "ingests")?.as_arr()?;
    let mut found = Vec::new();
    for item in arr {
        let Some(o) = item.as_obj() else { continue };
        let mut url = obj_str(o, "url_template").unwrap_or_default();
        url = url.replace("/{stream_key}", "");
        if !(url.starts_with("rtmp://") || url.starts_with("rtmps://")) {
            continue;
        }
        found.push(Ingest { name: obj_str(o, "name").unwrap_or_default(), url, priority: obj_i64(o, "priority").unwrap_or(0) });
    }
    if found.is_empty() {
        return None;
    }
    found.sort_by_key(|i| i.priority);
    Some(found)
}

fn replace_ingests(list: &mut [Service], name: &str, ingests: &[Ingest]) {
    let Some(svc) = list.iter_mut().find(|s| s.name == name) else { return };
    svc.servers.clear();
    if let Some(first) = ingests.first() {
        svc.servers.push(Server { name: "Auto".into(), url: first.url.clone() });
    }
    for ing in ingests {
        svc.servers.push(Server { name: ing.name.clone(), url: ing.url.clone() });
    }
}

fn read_profiles(services: &[Service]) -> Vec<Profile> {
    let Some(root) = obs_root() else { return Vec::new() };
    let dir = root.join("basic/profiles");
    let mut names: Vec<String> = fs::read_dir(&dir)
        .ok()
        .into_iter()
        .flatten()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_dir())
        .filter_map(|e| e.file_name().into_string().ok())
        .collect();
    names.sort();
    names.into_iter().filter_map(|name| read_profile(services, &dir.join(&name), name)).collect()
}

fn read_profile(services: &[Service], dir: &Path, name: String) -> Option<Profile> {
    if let Some(text) = fs::read_to_string(dir.join("service.json")).ok() {
        if let Some(v) = parse_json(&text) {
            if let Some(settings) = v.as_obj().and_then(|o| field(o, "settings")).and_then(|s| s.as_obj()) {
                let service = obj_str(settings, "service").unwrap_or_default();
                let server = obj_str(settings, "server").unwrap_or_default();
                let key = obj_str(settings, "key").unwrap_or_default();
                let (server, url) = resolve_server(services, &service, &server);
                return Some(Profile { name, service, server, url, key });
            }
        }
    }
    let ini = fs::read_to_string(dir.join("basic.ini")).unwrap_or_default();
    let service = ini_value(&ini, "Stream1", "Service");
    let server = ini_value(&ini, "Stream1", "Server");
    let key = ini_value(&ini, "Stream1", "key");
    if service.is_empty() && server.is_empty() && key.is_empty() {
        return Some(Profile { name, service, server, url: String::new(), key });
    }
    let (server, url) = resolve_server(services, &service, &server);
    Some(Profile { name, service, server, url, key })
}

fn resolve_server(services: &[Service], service: &str, server: &str) -> (String, String) {
    if server.starts_with("rtmp://") || server.starts_with("rtmps://") {
        return (String::new(), server.to_string());
    }
    let Some(svc) = services.iter().find(|s| s.name == service) else {
        return (server.to_string(), String::new());
    };
    if server.is_empty() || server.eq_ignore_ascii_case("auto") {
        if let Some(s) = svc.servers.first() {
            return ("Auto".into(), s.url.clone());
        }
    }
    if let Some(s) = svc.servers.iter().find(|s| s.name == server) {
        return (s.name.clone(), s.url.clone());
    }
    (server.to_string(), String::new())
}

fn current_profile_dir() -> Option<String> {
    let text = fs::read_to_string(obs_root()?.join("global.ini")).ok()?;
    let dir = ini_value(&text, "Basic", "ProfileDir");
    if dir.is_empty() { None } else { Some(dir) }
}

fn ini_value(text: &str, section: &str, key: &str) -> String {
    let mut in_section = false;
    let header = format!("[{section}]");
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_section = line.eq_ignore_ascii_case(&header);
            continue;
        }
        if !in_section {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            if k.trim().eq_ignore_ascii_case(key) {
                return v.trim().to_string();
            }
        }
    }
    String::new()
}

fn parse_services(text: &str) -> Option<Vec<Service>> {
    let v = parse_json(text)?;
    let arr = obj_get(&v, "services")?.as_arr()?;
    let mut list = Vec::new();
    for item in arr {
        let Some(o) = item.as_obj() else { continue };
        let name = obj_str(o, "name").unwrap_or_default();
        let mut servers = Vec::new();
        if let Some(arr) = field(o, "servers").and_then(|s| s.as_arr()) {
            for srv in arr {
                let Some(so) = srv.as_obj() else { continue };
                let url = obj_str(so, "url").unwrap_or_default();
                if url.starts_with("rtmp://") || url.starts_with("rtmps://") {
                    servers.push(Server { name: obj_str(so, "name").unwrap_or_default(), url });
                }
            }
        }
        if name.is_empty() || servers.is_empty() {
            continue;
        }
        let rec = field(o, "recommended").and_then(|r| r.as_obj());
        list.push(Service {
            name,
            common: obj_bool(o, "common"),
            key_link: obj_str(o, "stream_key_link").unwrap_or_default(),
            max_video_kbps: rec.and_then(|r| obj_i64(r, "max video bitrate")).unwrap_or(0),
            max_audio_kbps: rec.and_then(|r| obj_i64(r, "max audio bitrate")).unwrap_or(0),
            keyint_sec: rec.and_then(|r| obj_i64(r, "keyint")).unwrap_or(0),
            servers,
        });
    }
    Some(list)
}

fn esc(s: &str) -> String {
    let mut o = String::new();
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            c if c.is_control() => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o
}

#[derive(Clone)]
enum V {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<V>),
    Obj(Vec<(String, V)>),
}

impl V {
    fn as_obj(&self) -> Option<&[(String, V)]> {
        match self {
            V::Obj(o) => Some(o),
            _ => None,
        }
    }
    fn as_arr(&self) -> Option<&[V]> {
        match self {
            V::Arr(a) => Some(a),
            _ => None,
        }
    }
}

fn obj_get<'a>(v: &'a V, key: &str) -> Option<&'a V> {
    field(v.as_obj()?, key)
}

fn field<'a>(o: &'a [(String, V)], key: &str) -> Option<&'a V> {
    o.iter().find(|(k, _)| k == key).map(|(_, val)| val)
}

fn obj_str(o: &[(String, V)], key: &str) -> Option<String> {
    match o.iter().find(|(k, _)| k == key).map(|(_, v)| v) {
        Some(V::Str(s)) => Some(s.clone()),
        _ => None,
    }
}

fn obj_bool(o: &[(String, V)], key: &str) -> bool {
    matches!(o.iter().find(|(k, _)| k == key).map(|(_, v)| v), Some(V::Bool(true)))
}

fn obj_i64(o: &[(String, V)], key: &str) -> Option<i64> {
    match o.iter().find(|(k, _)| k == key).map(|(_, v)| v) {
        Some(V::Num(n)) => Some(*n as i64),
        _ => None,
    }
}

struct P<'a> {
    s: &'a [u8],
    i: usize,
}

fn parse_json(text: &str) -> Option<V> {
    let mut p = P { s: text.as_bytes(), i: 0 };
    let v = p.value()?;
    p.skip();
    if p.i != p.s.len() {
        return None;
    }
    Some(v)
}

impl<'a> P<'a> {
    fn skip(&mut self) {
        while let Some(c) = self.s.get(self.i) {
            if c.is_ascii_whitespace() {
                self.i += 1;
            } else {
                break;
            }
        }
    }
    fn eat(&mut self, c: u8) -> bool {
        self.skip();
        if self.s.get(self.i) == Some(&c) {
            self.i += 1;
            true
        } else {
            false
        }
    }
    fn value(&mut self) -> Option<V> {
        self.skip();
        match self.s.get(self.i).copied()? {
            b'n' => self.lit(b"null").then_some(V::Null),
            b't' => self.lit(b"true").then_some(V::Bool(true)),
            b'f' => self.lit(b"false").then_some(V::Bool(false)),
            b'"' => self.string().map(V::Str),
            b'[' => self.array(),
            b'{' => self.object(),
            b'-' | b'0'..=b'9' => self.number(),
            _ => None,
        }
    }
    fn lit(&mut self, s: &[u8]) -> bool {
        if self.s[self.i..].starts_with(s) {
            self.i += s.len();
            true
        } else {
            false
        }
    }
    fn string(&mut self) -> Option<String> {
        if !self.eat(b'"') {
            return None;
        }
        let mut o = String::new();
        while let Some(c) = self.s.get(self.i).copied() {
            self.i += 1;
            match c {
                b'"' => return Some(o),
                b'\\' => {
                    let e = self.s.get(self.i).copied()?;
                    self.i += 1;
                    match e {
                        b'"' => o.push('"'),
                        b'\\' => o.push('\\'),
                        b'/' => o.push('/'),
                        b'b' => o.push('\u{0008}'),
                        b'f' => o.push('\u{000c}'),
                        b'n' => o.push('\n'),
                        b'r' => o.push('\r'),
                        b't' => o.push('\t'),
                        b'u' => {
                            if self.i + 4 > self.s.len() {
                                return None;
                            }
                            let hex = std::str::from_utf8(&self.s[self.i..self.i + 4]).ok()?;
                            self.i += 4;
                            let u = u32::from_str_radix(hex, 16).ok()?;
                            o.push(char::from_u32(u).unwrap_or('\u{FFFD}'));
                        }
                        _ => return None,
                    }
                }
                c if c < 0x80 => o.push(c as char),
                _ => {
                    self.i -= 1;
                    let rest = std::str::from_utf8(&self.s[self.i..]).ok()?;
                    let ch = rest.chars().next()?;
                    self.i += ch.len_utf8();
                    o.push(ch);
                }
            }
        }
        None
    }
    fn number(&mut self) -> Option<V> {
        let start = self.i;
        if self.s.get(self.i) == Some(&b'-') {
            self.i += 1;
        }
        while matches!(self.s.get(self.i), Some(b'0'..=b'9')) {
            self.i += 1;
        }
        if self.s.get(self.i) == Some(&b'.') {
            self.i += 1;
            while matches!(self.s.get(self.i), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        if matches!(self.s.get(self.i), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.s.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            while matches!(self.s.get(self.i), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        let text = std::str::from_utf8(&self.s[start..self.i]).ok()?;
        Some(V::Num(text.parse().ok()?))
    }
    fn array(&mut self) -> Option<V> {
        if !self.eat(b'[') {
            return None;
        }
        let mut items = Vec::new();
        self.skip();
        if self.eat(b']') {
            return Some(V::Arr(items));
        }
        loop {
            items.push(self.value()?);
            self.skip();
            if self.eat(b']') {
                return Some(V::Arr(items));
            }
            if !self.eat(b',') {
                return None;
            }
        }
    }
    fn object(&mut self) -> Option<V> {
        if !self.eat(b'{') {
            return None;
        }
        let mut items = Vec::new();
        self.skip();
        if self.eat(b'}') {
            return Some(V::Obj(items));
        }
        loop {
            self.skip();
            let key = self.string()?;
            if !self.eat(b':') {
                return None;
            }
            items.push((key, self.value()?));
            self.skip();
            if self.eat(b'}') {
                return Some(V::Obj(items));
            }
            if !self.eat(b',') {
                return None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keeps_rtmp_servers_and_recommended() {
        let text = r#"{"services":[
            {"name":"Twitch","common":true,"stream_key_link":"https://dashboard.twitch.tv/settings/stream",
             "servers":[{"name":"EU","url":"rtmp://eu.example/app"},{"name":"HLS","url":"https://nope"}],
             "recommended":{"keyint":2,"max video bitrate":6000,"max audio bitrate":320}},
            {"name":"Only web","servers":[{"name":"x","url":"https://x"}]}
        ]}"#;
        let list = parse_services(text).unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].name, "Twitch");
        assert_eq!(list[0].servers.len(), 1);
        assert_eq!(list[0].max_video_kbps, 6000);
        assert!(list[0].common);
    }

    #[test]
    fn reads_obs_services_json_when_installed() {
        let Some(app) = std::env::var_os("APPDATA") else { return };
        let path = PathBuf::from(app).join("obs-studio/plugin_config/rtmp-services/services.json");
        if !path.exists() {
            return;
        }
        let text = fs::read_to_string(&path).unwrap();
        let list = parse_services(&text).expect("services.json");
        assert!(list.len() > 20, "parsed {}", list.len());
        assert!(list.iter().any(|s| s.name == "Twitch"));
        assert!(list.iter().any(|s| s.name.contains('ã') || s.servers.iter().any(|srv| srv.name.contains('ã'))));
    }

    #[test]
    fn ingest_list_replaces_servers_closest_first() {
        let mut list = parse_services(r#"{"services":[{"name":"Twitch","servers":[{"name":"Old","url":"rtmp://old/app"}]}]}"#).unwrap();
        let ing = vec![
            Ingest { name: "Warsaw".into(), url: "rtmp://waw.example/app".into(), priority: 1 },
            Ingest { name: "Far".into(), url: "rtmp://far.example/app".into(), priority: 9 },
        ];
        replace_ingests(&mut list, "Twitch", &ing);
        assert_eq!(list[0].servers[0].name, "Auto");
        assert_eq!(list[0].servers[0].url, "rtmp://waw.example/app");
        assert_eq!(list[0].servers[1].name, "Warsaw");
        assert_eq!(list[0].servers.len(), 3);
    }
}
