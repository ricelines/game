/** Actual purpose form, reload, native causal attribution and clickable root trace. */
import { chromium } from "@playwright/test";
import { WebSocket } from "ws";
import { readFileSync, writeFileSync, statSync, mkdirSync } from "node:fs";
import { resolve, join } from "node:path";

const root = resolve(import.meta.dirname, "../..");
const out = resolve(process.env.AMB_TEST_OUTPUT || join(root, ".cache/client"));
const residentReport =
  process.env.AMB_TEST_RESIDENT_REPORT || join(out, "ui-purpose-resident.json");
mkdirSync(out, { recursive: true });
const world = process.env.AMB_TEST_WORLD || "ws://127.0.0.1:18080/ws";
const url = process.env.AMB_TEST_URL || "http://127.0.0.1:18080/?quality=low";
const gateway = process.env.AMB_TEST_GATEWAY || "http://127.0.0.1:18881";
function key(path) {
  if ((statSync(path).mode & 0o777) !== 0o600)
    throw Error("Private browser credential requires mode0600");
  return readFileSync(path, "utf8").trim();
}
const watch = key(
  process.env.AMB_TEST_KEY_FILE || join(root, ".runtime/keys/spectator.key"),
);
const resident = key(
  process.env.AMB_TEST_RESIDENT_KEY_FILE || join(root, ".runtime/keys/rue.key"),
);
function redact(value) {
  return String(value)
    .replaceAll(watch, "[credential]")
    .replaceAll(resident, "[credential]");
}
function require(value, message) {
  if (!value) throw Error(message);
}
const report = {
  kind: "Manual actual native/gateway/browser purpose and causal UI; no model calls",
  checks: {},
  errors: [],
};
function checkpoint(stage) {
  report.stage = stage;
  writeFileSync(
    join(out, "ui-purpose-browser-report.json"),
    JSON.stringify(report, null, 2),
  );
}
let browser, page, observer;
try {
  browser = await chromium.launch({
    headless: true,
    args: [
      "--no-sandbox",
      "--enable-unsafe-swiftshader",
      "--use-angle=swiftshader",
    ],
  });
  page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  page.on("pageerror", (error) => report.errors.push(redact(error.message)));
  page.on("console", (message) => {
    if (message.type() === "error") report.errors.push(redact(message.text()));
  });
  await page.addInitScript(
    (config) => {
      window.ambergris_config = config;
    },
    {
      key: watch,
      purpose_token: watch,
      mode: "debug",
      gateway,
      ws: world,
      diagnostics: true,
    },
  );
  await page.goto(url, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(
    () =>
      window.ambergris?.connected &&
      window.ambergris?.ui?.purpose &&
      Object.keys(window.ambergris.ui.purposes).length === 1,
    undefined,
    { timeout: 90000 },
  );
  checkpoint("connected");
  const purpose =
    "Manual UI purpose persisted through the real resident context; model remains paused.";
  const fields = await page.evaluate(() => window.ambergris.ui.purpose);
  const center = (rect) => [rect[0] + rect[2] / 2, rect[1] + rect[3] / 2];
  await page.mouse.click(...center(fields.input));
  await page.keyboard.type(purpose, { delay: 10 });
  const response = page.waitForResponse(
    (row) =>
      row.url() === gateway + "/api/purpose" &&
      row.request().method() === "POST",
  );
  await page.mouse.click(...center(fields.send));
  const delivered = await response;
  require(delivered.status() ===
    200, "Actual purpose endpoint rejected form input");
  require(JSON.parse(delivered.request().postData()).purpose ===
    purpose, "The actual form did not send typed content");
  await page.waitForFunction(
    () => window.ambergris?.ui?.purpose?.status.includes("delivered"),
    undefined,
    { timeout: 10000 },
  );
  report.checks.purpose_form = await delivered.json();
  checkpoint("purpose_delivered");
  const persisted = JSON.parse(readFileSync(residentReport, "utf8"));
  require(persisted.agents[0].purpose === purpose &&
    persisted.responses.length ===
      0, "Actual resident checkpoint did not persist form input without a model call");
  report.checks.checkpoint = {
    purpose: persisted.agents[0].purpose,
    responses: persisted.responses.length,
  };
  await page.screenshot({ path: join(out, "ui-purpose-delivered.png") });
  await page.reload({ waitUntil: "domcontentloaded" });
  await page.waitForFunction(
    (expected) => window.ambergris?.ui?.purposes?.["p:6"] === expected,
    purpose,
    { timeout: 90000 },
  );
  report.checks.reload_reflects_purpose = true;
  checkpoint("purpose_reloaded");
  await page.screenshot({ path: join(out, "ui-purpose-reloaded.png") });

  let sequence = 0;
  const pending = new Map();
  observer = new WebSocket(world);
  let welcomeResolve;
  const welcomed = new Promise((resolve) => {
    welcomeResolve = resolve;
  });
  observer.on("message", (raw) => {
    const message = JSON.parse(raw.toString());
    if (message.t === "welcome") welcomeResolve(message);
    if (pending.has(message.id)) {
      pending.get(message.id)(message);
      pending.delete(message.id);
    }
  });
  await new Promise((resolve, reject) => {
    observer.once("open", resolve);
    observer.once("error", reject);
  });
  observer.send(
    JSON.stringify({
      t: "hello",
      version: 1,
      key: watch,
      mode: "debug",
      binary: false,
    }),
  );
  await welcomed;
  function query(t, fields) {
    const id = ++sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(Error("Actual native debug query timed out"));
      }, 20000);
      pending.set(id, (reply) => {
        clearTimeout(timer);
        resolve(reply);
      });
      observer.send(JSON.stringify({ t, id, ...fields }));
    });
  }
  const agents = await fetch(gateway + "/api/agents", {
    headers: { Authorization: "Bearer " + watch },
  }).then((row) => row.json());
  const body = agents.agents[0].body;
  const changed = await fetch(gateway + "/tools/do", {
    method: "POST",
    headers: {
      Authorization: "Bearer " + resident,
      "Content-Type": "application/json",
      "X-Ambergris-Session": "resident-p:6",
    },
    body: JSON.stringify({
      action: "base.make_material",
      args: { material: "base.wood", kg: 0.001 },
    }),
  }).then((row) => row.json());
  require(changed.ok, "Normal manual agent action did not apply");
  const recent = await query("events", {
    thing: body,
    since: 0,
    limit: 200,
    detail: false,
  });
  const action = recent.events
    .filter(
      (row) =>
        row.kind === "action" &&
        row.name === "base.make_material" &&
        row.actor === "p:6",
    )
    .at(-1);
  require(action, "Actual native stream has no chosen action record");
  const chain = await query("events", {
    root: action.root,
    since: 0,
    limit: 200,
    detail: false,
  });
  const handler = chain.events.find(
    (row) => row.kind === "handler" && row.cause === action.id && row.changed,
  );
  require(handler &&
    handler.authors.length >
      0, "Actual stream has no attributed effectful authored handler");
  report.checks.actual_records = chain.events;
  checkpoint("native_causal_records");

  const follow = await page.evaluate(() =>
    window.ambergris.ui.controls.find(
      (row) => row.kind === "button" && row.text === "Follow UI check resident",
    ),
  );
  await page.mouse.click(...center(follow.rect));
  await page.waitForFunction(
    (expected) =>
      window.ambergris?.selected === expected &&
      window.ambergris.inspection_ready,
    body,
    { timeout: 15000 },
  );
  checkpoint("body_selected");
  const trace = await page.evaluate(() =>
    window.ambergris.ui.controls.find((row) => row.text === "Trace effects"),
  );
  await page.mouse.click(...center(trace.rect));
  await page.mouse.click(177, 154);
  await page.waitForFunction(
    (id) => window.ambergris?.ui?.record?.text.includes("#" + id),
    action.id,
    { timeout: 15000 },
  );
  const record = await page.evaluate(() => window.ambergris.ui.record);
  checkpoint("record_rendered");
  require(record.text.includes("root #" + action.root) &&
    record.text.includes(
      "cause #" + action.id,
    ), "Rendered record omits actual root/cause");
  require(record.text.includes(
    "authors: " + handler.authors.join(", "),
  ), "Rendered record omits actual native authors");
  const link = record.links.find((row) => row.id === action.id);
  require(link, "Actual rendered action link has no measured text layout");
  await page.mouse.move(150, 400);
  if (link.point[1] > 720) await page.mouse.wheel(0, link.point[1] - 650);
  await page.waitForTimeout(700);
  const currentLink = await page.evaluate(
    (id) => window.ambergris.ui.record.links.find((row) => row.id === id),
    action.id,
  );
  await page.mouse.click(...currentLink.point);
  await page.waitForFunction(
    (root) =>
      window.ambergris?.trace_root === root &&
      window.ambergris?.ui?.record?.text.startsWith("CAUSAL CHAIN #" + root),
    action.root,
    { timeout: 15000 },
  );
  report.checks.causal_display = await page.evaluate(() => ({
    root: window.ambergris.trace_root,
    text: window.ambergris.ui.record.text,
  }));
  require(report.checks.causal_display.text.includes(
    handler.name,
  ), "Clicked causal root did not display its actual handler");
  await page.screenshot({ path: join(out, "ui-causal-trace.png") });
  require(report.errors.length === 0, "Browser reported errors");
  report.status = "passed";
} catch (error) {
  report.status = "failed";
  report.problem = redact(error);
  if (page) {
    await page
      .screenshot({ path: join(out, "ui-purpose-failure.png") })
      .catch(() => {});
    report.ui = await page.evaluate(() => window.ambergris).catch(() => null);
  }
  process.exitCode = 1;
} finally {
  if (observer) observer.close();
  if (browser) await browser.close();
  writeFileSync(
    join(out, "ui-purpose-browser-report.json"),
    JSON.stringify(report, null, 2),
  );
  console.log(
    JSON.stringify({
      status: report.status,
      problem: report.problem,
      errors: report.errors,
      evidence: join(out, "ui-purpose-browser-report.json"),
    }),
  );
}
