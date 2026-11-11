//! Opening the RTMP and API ports. Another RTMP server, a second OBS or a
//! Windows reserved range can hold the configured port; then the next free one
//! is taken and written to the ini, so OBS and the dock follow it.

use std::io;
use std::net::TcpListener;
use std::path::Path;

/// How many ports above the configured one are tried.
const TRIES: u16 = 50;

/// Binds `addr` (`host:port`), or the first free port above it. Returns the
/// listener (non-blocking, ready for tokio) and the address it really holds.
pub fn bind_free(addr: &str) -> io::Result<(TcpListener, String)> {
    let (host, port) = addr
        .rsplit_once(':')
        .and_then(|(h, p)| Some((h, p.trim().parse::<u16>().ok()?)))
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, format!("bad address {addr}")))?;
    let mut first_err: Option<io::Error> = None;
    for p in (0..TRIES).filter_map(|i| port.checked_add(i)) {
        let at = format!("{host}:{p}");
        match TcpListener::bind(&at) {
            Ok(l) => {
                l.set_nonblocking(true)?;
                if let Some(e) = &first_err {
                    log!("port {port} is taken by another program ({e}), using {p}");
                }
                return Ok((l, at));
            }
            // 10048 in use; 10013 a range Windows (Hyper-V, WSL) keeps for itself.
            Err(e) if matches!(e.kind(), io::ErrorKind::AddrInUse | io::ErrorKind::PermissionDenied) => {
                first_err.get_or_insert(e);
            }
            Err(e) => return Err(e),
        }
    }
    Err(first_err.unwrap_or_else(|| io::Error::new(io::ErrorKind::AddrInUse, addr.to_string())))
}

/// Saves the ports really in use, keeping the rest of the file as it is.
pub fn write_back(ini: &Path, listen: &str, api: &str) {
    if ini.as_os_str().is_empty() {
        return;
    }
    let Ok(text) = std::fs::read_to_string(ini) else { return };
    let upd = crate::config::IniUpdate { listen: Some(listen.into()), api: Some(api.into()), ..Default::default() };
    match std::fs::write(ini, crate::config::update_ini(&text, &upd)) {
        Ok(()) => log!("settings: listen = {listen}, api = {api}"),
        Err(e) => log!("could not save the new ports: {e}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_taken_port_moves_to_the_next_free_one() {
        // Port 0 means "any": ask the OS which one it gave, then ask for it again.
        let (held, _) = bind_free("127.0.0.1:0").unwrap();
        let port = held.local_addr().unwrap().port();
        let (_l, got) = bind_free(&format!("127.0.0.1:{port}")).unwrap();
        assert_ne!(got, format!("127.0.0.1:{port}"));
    }

    #[test]
    fn a_free_port_is_kept() {
        let (l, _) = bind_free("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        drop(l);
        let (_l, got) = bind_free(&format!("127.0.0.1:{port}")).unwrap();
        assert_eq!(got, format!("127.0.0.1:{port}"));
    }
}
