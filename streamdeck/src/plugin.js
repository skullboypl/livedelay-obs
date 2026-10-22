const { WebSocket } = require("ws");
const http = require("http");

function flag(name) {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] : "";
}

const port = flag("-port");
const uuid = flag("-pluginUUID");
const registerEvent = flag("-registerEvent") || "registerPlugin";

function apiBase(settings) {
  let api = String((settings && settings.api) || "127.0.0.1:7799").trim();
  api = api.replace(/^https?:\/\//i, "").replace(/\/+$/, "");
  if (!api) api = "127.0.0.1:7799";
  return "http://" + api;
}

function actionUrl(action, settings) {
  const base = apiBase(settings);
  if (action.endsWith(".arm")) {
    let delay = Number(settings && settings.delay);
    if (!Number.isFinite(delay) || delay < 1) delay = 15;
    if (delay > 3600) delay = 3600;
    return base + "/arm?delay=" + encodeURIComponent(delay);
  }
  if (action.endsWith(".activate")) return base + "/activate";
  if (action.endsWith(".cutafter")) return base + "/cut-after";
  if (action.endsWith(".cut")) return base + "/cut";
  if (action.endsWith(".disarm")) return base + "/disarm";
  return "";
}

function armTitle(settings) {
  let delay = Number(settings && settings.delay);
  if (!Number.isFinite(delay) || delay < 1) delay = 15;
  return "ARM\n" + Math.round(delay) + "s";
}

function get(url) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, (res) => {
      res.resume();
      resolve(res.statusCode || 0);
    });
    req.setTimeout(4000, () => {
      req.destroy(new Error("timeout"));
    });
    req.on("error", reject);
  });
}

function connect() {
  if (!port || !uuid) {
    console.error("LiveDelay Stream Deck: missing -port or -pluginUUID");
    process.exit(1);
  }
  const ws = new WebSocket("ws://127.0.0.1:" + port);

  function send(msg) {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
  }

ws.on("open", () => {
  send({ event: registerEvent, uuid });
});

ws.on("message", async (raw) => {
  let msg;
  try {
    msg = JSON.parse(raw.toString());
  } catch {
    return;
  }
  const settings = (msg.payload && msg.payload.settings) || {};
  if (msg.event === "willAppear" || msg.event === "didReceiveSettings") {
    if (String(msg.action || "").endsWith(".arm")) {
      send({ event: "setTitle", context: msg.context, payload: { title: armTitle(settings), target: 0 } });
    }
    return;
  }
  if (msg.event !== "keyDown") return;
  const url = actionUrl(msg.action || "", settings);
  if (!url) return;
  try {
    const code = await get(url);
    send({ event: code >= 200 && code < 300 ? "showOk" : "showAlert", context: msg.context });
  } catch (err) {
    console.error("LiveDelay request failed", url, err && err.message);
    send({ event: "showAlert", context: msg.context });
  }
});

  ws.on("error", (err) => {
    console.error("LiveDelay websocket", err && err.message);
  });
}

if (require.main === module) connect();

module.exports = { apiBase, actionUrl, armTitle, get };
