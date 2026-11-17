/** Actual-server browser integration when configured; otherwise labelled renderer fixtures. */
import { chromium } from "@playwright/test";
import { WebSocketServer } from "ws";
import { createServer } from "node:http";
import {
  readFileSync,
  existsSync,
  mkdirSync,
  writeFileSync,
  readdirSync,
  statSync,
} from "node:fs";
import { resolve, join, extname } from "node:path";

const root = resolve(import.meta.dirname, "../..");
const externalURL = process.env.AMB_TEST_URL;
const label =
  process.env.AMB_TEST_LABEL || (externalURL ? "integration" : "fixture");
const assets = resolve(
  process.env.AMB_TEST_ASSETS || join(root, ".cache/benchmark-assets"),
);
const hashes = existsSync(assets)
  ? readdirSync(assets).filter((h) => existsSync(join(assets, h, "lod2.glb")))
  : [];
if (!externalURL && !hashes.length)
  throw Error(
    "Build an actual asset first; AMB_TEST_ASSETS points at its store.",
  );
const hash = hashes[0];
const configuration = JSON.parse(process.env.AMB_TEST_CONFIG || "{}");
const keyPath = process.env.AMB_TEST_KEY_FILE;
if (keyPath) {
  if ((statSync(keyPath).mode & 0o777) !== 0o600)
    throw Error("Browser credential file requires mode 0600");
  configuration.key = readFileSync(keyPath, "utf8").trim();
}
function redact(message) {
  return configuration.key
    ? message.replaceAll(configuration.key, "[credential]")
    : message;
}
const port = 8765;
const out = join(root, ".cache/client");
mkdirSync(out, { recursive: true });
const mime = {
  ".html": "text/html",
  ".js": "text/javascript",
  ".wasm": "application/wasm",
  ".pck": "application/octet-stream",
  ".json": "application/json",
  ".glb": "model/gltf-binary",
  ".png": "image/png",
  ".svg": "image/svg+xml",
};
const server = createServer((req, res) => {
  const pathname = new URL(req.url, "http://localhost").pathname;
  const base = pathname.startsWith("/assets/")
    ? assets
    : join(root, "client/dist");
  const suffix = pathname.startsWith("/assets/")
    ? pathname.slice(8)
    : pathname === "/"
      ? "index.html"
      : pathname;
  const file = resolve(
    base,
    "." + (suffix.startsWith("/") ? suffix : "/" + suffix),
  );
  if (!file.startsWith(base + "/") || !existsSync(file)) {
    res.writeHead(404).end();
    return;
  }
  const gzip =
    req.headers["accept-encoding"]?.includes("gzip") &&
    existsSync(file + ".gz");
  res.writeHead(200, {
    "Content-Type": mime[extname(file)] || "application/octet-stream",
    "Access-Control-Allow-Origin": "*",
    ...(gzip ? { "Content-Encoding": "gzip" } : {}),
  });
  res.end(readFileSync(file + (gzip ? ".gz" : "")));
});
const wss = new WebSocketServer({ server });
let step = 1;
const handles = [];
function quat() {
  return (3 | (512 << 2) | (512 << 12) | (512 << 22)) >>> 0;
}
function entity(i) {
  const b = Buffer.alloc(2 + 8 + 32 + 8 + 1 + 12 + 4 + 1 + 12);
  let n = 0;
  b.writeUInt16LE(i, n);
  n += 2;
  b.writeBigUInt64LE((1n << 32n) | BigInt(i), n);
  n += 8;
  Buffer.from(hash, "hex").copy(b, n);
  n += 32;
  b.writeBigUInt64LE(0n, n);
  n += 8;
  b[n++] = 0;
  b.writeInt32LE((((i - 1) % 15) - 7) * 75, n);
  n += 4;
  b.writeInt32LE((Math.floor((i - 1) / 15) - 7) * 75, n);
  n += 4;
  b.writeInt32LE(0, n);
  n += 4;
  b.writeUInt32LE(quat(), n);
  n += 4;
  b[n++] = 2;
  b.writeUInt16LE(1, n);
  n += 2;
  b.writeFloatLE((i % 10) / 10, n);
  n += 4;
  b.writeUInt16LE(2, n);
  n += 2;
  b.writeFloatLE(1, n);
  return b;
}
function keyframe() {
  const b = Buffer.alloc(11);
  b[0] = 1;
  b.writeInt16LE(0, 1);
  b.writeInt16LE(0, 3);
  b.writeUInt32LE(step, 5);
  b.writeUInt16LE(225, 9);
  return Buffer.concat([
    b,
    ...Array.from({ length: 225 }, (_, i) => entity(i + 1)),
  ]);
}
wss.on("connection", (ws) => {
  ws.on("message", (raw) => {
    const m = JSON.parse(raw.toString());
    if (m.t === "hello") {
      ws.send(
        JSON.stringify({
          t: "welcome",
          session: 1,
          player: "p:1",
          body: "o:1.1",
          step,
          step_hz: 20,
          repl_delay_ms: 150,
          cell_m: 32,
          assets: "/assets/",
        }),
      );
      ws.send(
        JSON.stringify({
          t: "catalog",
          terms: [
            { id: 1, name: "fixture.arbitrary", unit: "fraction" },
            { id: 2, name: "fixture.visible" },
          ],
          materials: [{ id: 1, color: [0.65, 0.47, 0.25, 1], roughness: 0.5 }],
          models: [
            {
              id: 1,
              asset: hash,
              material: 1,
              text: {
                name: "Baked renderer fixture",
                description:
                  "Actual immutable CAD mesh; synthetic protocol fixture.",
              },
              bindings: {
                render: [
                  {
                    term: "fixture.arbitrary",
                    property: "color",
                    colors: [
                      [0.2, 0.4, 0.7, 1],
                      [1, 0.8, 0.4, 1],
                    ],
                  },
                ],
              },
            },
          ],
          world: { ambient_lux: 85 },
        }),
      );
      ws.send(
        JSON.stringify({
          t: "metadata",
          objects: Array.from({ length: 225 }, (_, i) => ({
            h: `o:${i + 1}.1`,
            model: 1,
            material: 1,
            owner: "p:1",
          })),
        }),
      );
      ws.send(keyframe());
    } else if (m.t === "inspect")
      ws.send(
        JSON.stringify({
          t: "inspection",
          id: m.id,
          measured: JSON.parse(
            readFileSync(join(assets, hash, "manifest.json")),
          ),
          says: { name: "Actual baked artifact" },
        }),
      );
    else if (m.t === "events")
      ws.send(
        JSON.stringify({
          t: "events",
          id: m.id,
          events: [
            {
              id: 1,
              root: 1,
              cause: null,
              kind: "fixture",
              name: "Renderer fixture connected",
              actor: "p:1",
              things: ["o:1.1"],
              result: "test",
            },
          ],
        }),
      );
  });
});
if (!externalURL) await new Promise((r) => server.listen(port, "127.0.0.1", r));
let browser;
let page;
try {
  browser = await chromium.launch({
    headless: true,
    args: [
      "--no-sandbox",
      "--enable-unsafe-swiftshader",
      "--use-angle=swiftshader",
    ],
  });
  page = await browser.newPage({
    viewport: { width: 1440, height: 900 },
  });
  const errors = [];
  const messages = [];
  await page.addInitScript((config) => {
    window.ambergris_config = config;
  }, configuration);
  page.on("pageerror", (e) => errors.push(redact(e.message)));
  page.on("console", (m) => {
    if (m.type() === "error") errors.push(redact(m.text()));
    messages.push(redact(m.text()));
  });
  const assetDelayMs = Number(process.env.AMB_TEST_ASSET_DELAY_MS || 0);
  if (assetDelayMs)
    await page.route("**/assets/*/lod*.glb", async (route) => {
      await new Promise((resolve) => setTimeout(resolve, assetDelayMs));
      await route.continue();
    });
  const began = Date.now();
  await page.goto(externalURL || `http://127.0.0.1:${port}/?quality=low`, {
    waitUntil: "domcontentloaded",
  });
  let loading = null;
  if (assetDelayMs) {
    await page.waitForFunction(
      () => window.ambergris?.loading?.measured > 0,
      undefined,
      { timeout: 90000 },
    );
    loading = await page.evaluate(() => window.ambergris);
    await page.screenshot({ path: join(out, label + "-loading.png") });
  }
  await page.waitForFunction(
    (minimum) =>
      window.ambergris?.entities >= minimum &&
      window.ambergris?.asset_bytes > 0 &&
      window.ambergris?.groups > 0,
    Number(process.env.AMB_TEST_MIN_OBJECTS || (externalURL ? 1 : 225)),
    { timeout: 90000 },
  );
  if (assetDelayMs)
    await page.waitForFunction(
      () =>
        window.ambergris?.loading?.measured === 0 &&
        window.ambergris?.loading?.neutral === 0,
      undefined,
      { timeout: 90000 },
    );
  const joinedMs = Date.now() - began;
  await page.waitForTimeout(3000);
  await page.screenshot({ path: join(out, label + ".png") });
  if (process.env.AMB_TEST_FOLLOW_SEARCH) {
    await page.mouse.click(150, 267);
    await page.keyboard.type(process.env.AMB_TEST_FOLLOW_SEARCH);
    await page.waitForTimeout(700);
    await page.mouse.click(150, 322);
    await page.waitForFunction(
      () => window.ambergris?.selected && window.ambergris?.inspection_ready,
      undefined,
      { timeout: 15000 },
    );
    await page.mouse.click(1200, 212);
  } else {
    const followPoint = JSON.parse(
      process.env.AMB_TEST_FOLLOW_POINT || "[140,373]",
    );
    await page.mouse.click(...followPoint);
  }
  await page.waitForFunction(
    () =>
      window.ambergris?.following &&
      window.ambergris?.selected === window.ambergris?.following &&
      window.ambergris?.inspection_ready,
    undefined,
    { timeout: 15000 },
  );
  const followed = await page.evaluate(() => window.ambergris.following);
  await page.screenshot({ path: join(out, label + "-follow.png") });
  const profiler = process.env.AMB_TEST_PROFILE
    ? await page.context().newCDPSession(page)
    : null;
  if (profiler) {
    await profiler.send("Profiler.enable");
    await profiler.send("Profiler.start");
  }
  const beforeFrames = await page.evaluate(() => window.ambergris);
  const frameBegan = Date.now();
  const frames = await page.evaluate(
    () =>
      new Promise((resolve) => {
        const times = [];
        let last = performance.now();
        const until = last + 5000;
        function f(now) {
          times.push(now - last);
          last = now;
          if (now < until) requestAnimationFrame(f);
          else resolve(times);
        }
        requestAnimationFrame(f);
      }),
  );
  frames.sort((a, b) => a - b);
  const afterFrames = await page.evaluate(() => window.ambergris);
  if (profiler) {
    const { profile } = await profiler.send("Profiler.stop");
    writeFileSync(join(out, label + "-profile.json"), JSON.stringify(profile));
    await profiler.detach();
  }
  const replicationBytesPerSecond =
    ((afterFrames.replication_bytes - beforeFrames.replication_bytes) * 1000) /
    (Date.now() - frameBegan);
  const gpu = await page.evaluate(() => {
    const gl = document.getElementById("canvas").getContext("webgl2");
    const debug = gl.getExtension("WEBGL_debug_renderer_info");
    return debug
      ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL)
      : "unavailable";
  });
  const report = {
    gpu,
    kind: externalURL
      ? "actual configured server/browser integration"
      : "renderer fixture, synthetic S6 rows and real baked GLB; not world integration",
    joinedMs,
    loading,
    assetDelayMs,
    replicationBytesPerSecond,
    followed,
    stats: await page.evaluate(() => window.ambergris),
    frameMs: {
      p50: frames[Math.floor(frames.length * 0.5)],
      p95: frames[Math.floor(frames.length * 0.95)],
      samples: frames.length,
    },
    errors,
    messages,
    asset: externalURL ? undefined : hash,
  };
  await page.mouse.click(1105, 50);
  await page.waitForFunction(
    () => window.ambergris?.following === "",
    undefined,
    { timeout: 10000 },
  );
  await page.mouse.click(1290, 50);
  await page.waitForFunction(
    () => window.ambergris?.debug === true,
    undefined,
    { timeout: 10000 },
  );
  await page.screenshot({ path: join(out, label + "-debug.png") });
  report.controls = {
    follow: true,
    inspect: true,
    freeCamera: true,
    debug: true,
  };
  writeFileSync(
    join(out, label + "-report.json"),
    JSON.stringify(report, null, 2),
  );
  console.log(JSON.stringify({ ...report, messages: undefined }, null, 2));
  if (errors.length) process.exitCode = 1;
} catch (error) {
  console.error(redact(String(error)));
  if (page) {
    await page
      .screenshot({ path: join(out, label + "-failure.png"), timeout: 5000 })
      .catch(() => {});
    console.error(
      JSON.stringify(
        await page.evaluate(() => window.ambergris).catch(() => null),
      ),
    );
  }
  process.exitCode = 1;
} finally {
  if (browser) await browser.close();
  for (const ws of wss.clients) ws.terminate();
  wss.close();
  if (server.listening) await new Promise((r) => server.close(r));
}
