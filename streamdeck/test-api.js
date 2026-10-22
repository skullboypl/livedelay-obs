// Exercises the Stream Deck plugin's request builder against a local stand-in for LiveDelay.
const assert = require("assert");
const http = require("http");
const { apiBase, actionUrl, armTitle, get } = require("./src/plugin.js");

assert.strictEqual(apiBase({}), "http://127.0.0.1:7799");
assert.strictEqual(apiBase({ api: "http://127.0.0.1:7800/" }), "http://127.0.0.1:7800");
assert.strictEqual(apiBase({ api: "  " }), "http://127.0.0.1:7799");

assert.strictEqual(actionUrl("com.skullmedia.livedelay.arm", { delay: 30 }), "http://127.0.0.1:7799/arm?delay=30");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.arm", { delay: 0 }), "http://127.0.0.1:7799/arm?delay=15");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.arm", { delay: 99999 }), "http://127.0.0.1:7799/arm?delay=3600");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.arm", { api: "127.0.0.1:7801", delay: 25 }), "http://127.0.0.1:7801/arm?delay=25");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.activate", {}), "http://127.0.0.1:7799/activate");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.cutafter", {}), "http://127.0.0.1:7799/cut-after");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.cut", {}), "http://127.0.0.1:7799/cut");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.disarm", {}), "http://127.0.0.1:7799/disarm");
assert.strictEqual(actionUrl("com.skullmedia.livedelay.nope", {}), "");
assert.strictEqual(armTitle({ delay: 30 }), "ARM\n30s");
assert.strictEqual(armTitle({}), "ARM\n15s");

const hits = [];
const server = http.createServer((req, res) => {
  hits.push(req.url);
  const code = req.url.startsWith("/arm") ? 200 : req.url === "/activate" ? 409 : 404;
  res.writeHead(code);
  res.end();
});

server.listen(0, "127.0.0.1", async () => {
  const { port } = server.address();
  const base = "http://127.0.0.1:" + port;
  try {
    assert.strictEqual(await get(base + "/arm?delay=30"), 200);
    assert.strictEqual(await get(base + "/activate"), 409);
    assert.strictEqual(await get(base + "/missing"), 404);
    assert.deepStrictEqual(hits, ["/arm?delay=30", "/activate", "/missing"]);
    console.log("streamdeck api tests ok");
  } finally {
    server.close();
  }
});
