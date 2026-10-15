// Run with: node --test tests/test_mqtt_web.cjs (Node.js 18+).
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const app = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
const start = app.indexOf('async function requestMqttCommand(');
assert.ok(start >= 0);
const commandCode = app.slice(start);
const cases = [
  { name: 'snapshot success', status: 200, json: { ok: true }, ok: true, text: '已提交' },
  { name: 'pause success', cmd: 'pause', status: 200, json: { ok: true }, ok: true, text: '已提交' },
  { name: 'HTTP 503', status: 503, json: { ok: false, error: 'mqtt unavailable' }, text: 'HTTP 503' },
  { name: 'HTTP failure overrides ok true', status: 500, json: { ok: true }, text: 'HTTP 500' },
  { name: 'application rejection', status: 200, json: { ok: false, error: 'rejected' }, text: 'rejected' },
  { name: 'original unescaped JSON', status: 200, raw: '{"ok":true,"sent":"{"cmd":"snapshot"}"}', text: '响应格式异常' },
  { name: 'empty body', status: 200, raw: '', text: '响应格式异常' },
  { name: 'null body', status: 200, json: null, text: 'ok 字段' },
  { name: 'missing ok', status: 200, json: {}, text: 'ok 字段' },
  { name: 'string ok', status: 200, json: { ok: 'false' }, text: 'ok 字段' },
  { name: 'HTML error', status: 502, raw: '<html>Bad Gateway</html>', text: 'HTTP 502' },
  { name: 'connection failure', network: true, text: '命令结果未知' },
];
for (const c of cases) test(c.name, async () => {
  let calls = 0;
  const toasts = [];
  const ctx = vm.createContext({
    fetch: async (url, options) => {
      calls++;
      assert.equal(url, '/api/mqtt/cmd');
      assert.equal(options.method, 'POST');
      assert.deepEqual(JSON.parse(options.body), { device: 'A1', cmd: c.cmd || 'snapshot' });
      if (c.network) throw new TypeError('Failed to fetch');
      return {
        ok: c.status >= 200 && c.status < 300, status: c.status,
        json: async () => c.raw !== undefined ? JSON.parse(c.raw) : c.json
      };
    },
    mqCmdLog: [], escHtml: s => s,
    toast: (message, ok) => toasts.push({ message, ok }),
    refreshDevice: () => {}, setTimeout: () => {},
    console: { error: () => {} }
  });
  vm.runInContext(commandCode, ctx);
  await ctx.mqCmd(c.cmd || 'snapshot');
  assert.equal(calls, 1, 'do not retry a command automatically');
  assert.equal(toasts.length, 1);
  assert.equal(toasts[0].ok, !!c.ok);
  assert.ok(toasts[0].message.includes(c.text), toasts[0].message);
  assert.equal(ctx.mqCmdLog.length, 1);
});
