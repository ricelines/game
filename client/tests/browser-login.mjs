/** Bare-origin spectator entry against the actual native server; no injected credential. */
import { chromium } from "@playwright/test";
import { readFileSync, writeFileSync, statSync, mkdirSync } from "node:fs";
import { resolve, join } from "node:path";

const root = resolve(import.meta.dirname, "../..");
const out = resolve(
  process.env.AMB_TEST_OUTPUT || join(root, ".cache/client/login"),
);
const url = process.env.AMB_TEST_URL || "http://127.0.0.1:18080/";
const keyFile =
  process.env.AMB_TEST_KEY_FILE || join(root, ".runtime/keys/spectator.key");
if ((statSync(keyFile).mode & 0o777) !== 0o600)
  throw Error("Expected private mode0600 key file");
const credential = readFileSync(keyFile, "utf8").trim();
const redact = (value) => String(value).replaceAll(credential, "[credential]");
mkdirSync(out, { recursive: true });
const report = {
  kind: "Actual bare-URL spectator credential UI; no configured/injected credential or model calls",
  url,
  errors: [],
  checks: {},
  hello: { acceptedCredential: 0, rejectedCredential: 0 },
};
function require(value, message) {
  if (!value) throw Error(message);
}
function checkpoint(stage) {
  report.stage = stage;
  writeFileSync(
    join(out, "login-report.json"),
    JSON.stringify(report, null, 2),
  );
}
const center = (rect) => [rect[0] + rect[2] / 2, rect[1] + rect[3] / 2];
let browser, page;
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
  page.on("websocket", (socket) => {
    socket.on("framesent", ({ payload }) => {
      if (typeof payload !== "string") return;
      const value = JSON.parse(payload);
      if (value.t !== "hello") return;
      report.hello[
        value.key === credential ? "acceptedCredential" : "rejectedCredential"
      ] += 1;
    });
  });
  // Only layout diagnostics are enabled. Authentication still uses the rendered form.
  await page.addInitScript(() => {
    window.ambergris_config = { diagnostics: true };
  });
  const started = Date.now();
  await page.goto(url, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(
    () => window.ambergris?.ui?.connection,
    undefined,
    { timeout: 90000 },
  );
  require(!(await page.evaluate(
    () => window.ambergris.connected,
  )), "Bare URL bypassed credential entry");
  require(report.hello.acceptedCredential + report.hello.rejectedCredential ===
    0, "Missing credential caused an automatic authentication attempt");
  await page.screenshot({ path: join(out, "spectator-entry.png") });
  checkpoint("entry");
  let entry = await page.evaluate(() => window.ambergris.ui.connection);
  await page.mouse.click(...center(entry.input));
  await page.keyboard.type("invalid-spectator-key");
  await page.waitForFunction(
    () => window.ambergris?.ui?.connection?.ready,
    undefined,
    { timeout: 15000 },
  );
  entry = await page.evaluate(() => window.ambergris.ui.connection);
  await page.mouse.click(...center(entry.connect));
  await page.waitForFunction(
    () => window.ambergris?.ui?.connection?.status.includes("not accepted"),
    undefined,
    { timeout: 15000 },
  );
  await page.waitForTimeout(2500);
  require(report.hello.rejectedCredential ===
    1, "Rejected key kept retrying automatically");
  entry = await page.evaluate(() => window.ambergris.ui.connection);
  await page.mouse.click(...center(entry.input));
  await page.keyboard.type(credential);
  await page.keyboard.press("Enter");
  await page.waitForFunction(
    () =>
      window.ambergris?.connected &&
      window.ambergris.entities > 300 &&
      window.ambergris.assets > 15 &&
      window.ambergris.groups > 0 &&
      window.ambergris.loading.neutral + window.ambergris.loading.measured ===
        0,
    undefined,
    { timeout: 90000 },
  );
  report.joinedMs = Date.now() - started;
  require(report.hello.acceptedCredential ===
    1, "Typed credential did not authenticate exactly once");
  report.stats = await page.evaluate(() => window.ambergris);
  checkpoint("authenticated");
  // Explore is ready after authentication for inspection and following.
  await page.waitForFunction(
    () =>
      window.ambergris.ui.controls.some((control) => control.kind === "input"),
    undefined,
    { timeout: 15000 },
  );
  const search = await page.evaluate(() =>
    window.ambergris.ui.controls.find((control) => control.kind === "input"),
  );
  await page.mouse.click(...center(search.rect));
  await page.keyboard.type("Embodied resident", { delay: 10 });
  await page.waitForFunction(
    () =>
      window.ambergris.ui.controls.some(
        (control) =>
          control.kind === "button" && control.text === "Embodied resident",
      ),
    undefined,
    { timeout: 15000 },
  );
  const object = await page.evaluate(() =>
    window.ambergris.ui.controls.find(
      (control) =>
        control.kind === "button" && control.text === "Embodied resident",
    ),
  );
  await page.mouse.click(...center(object.rect));
  await page.waitForFunction(
    () => window.ambergris.inspection_ready,
    undefined,
    { timeout: 15000 },
  );
  const follow = await page.evaluate(() =>
    window.ambergris.ui.controls.find(
      (control) => control.kind === "button" && control.text === "Follow",
    ),
  );
  require(follow, "Actual object follow control is absent");
  await page.mouse.click(...center(follow.rect));
  await page.waitForFunction(
    () => window.ambergris?.following && window.ambergris.inspection_ready,
    undefined,
    { timeout: 15000 },
  );
  await page.screenshot({ path: join(out, "spectator-connected.png") });
  report.stats = await page.evaluate(() => window.ambergris);
  await page.mouse.click(1105, 50);
  await page.waitForFunction(
    () => window.ambergris.following === "",
    undefined,
    { timeout: 15000 },
  );
  await page.mouse.click(1208, 50);
  require(!JSON.stringify(report).includes(credential) &&
    !page
      .url()
      .includes(credential), "Credential leaked into diagnostic report or URL");
  report.checks = {
    entry: true,
    noDefaultHandshake: true,
    rejectedKeyCorrection: true,
    loadedActualAssets: true,
    follow: true,
    inspect: true,
    freeCamera: true,
    home: true,
  };
  require(report.errors.length === 0, "Browser reported errors");
  report.status = "passed";
  checkpoint("completed");
  console.log(
    JSON.stringify({
      status: report.status,
      errors: report.errors,
      evidence: join(out, "login-report.json"),
    }),
  );
} catch (error) {
  report.status = "failed";
  report.problem = redact(error);
  checkpoint("failed");
  console.error(report.problem);
  process.exitCode = 1;
} finally {
  if (browser) await browser.close();
}
