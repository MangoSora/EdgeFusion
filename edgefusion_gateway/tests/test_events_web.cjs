const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const app = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
const code = app.slice(app.indexOf('let evPage ='), app.indexOf('async function renderSettings'));
function setup() {
  const nodes = { '#ev-list': {innerHTML:''}, '#ev-pager': {innerHTML:''},
    '#ev-type-filter': {value:''} };
  const requests = [];
  const ctx = vm.createContext({
    $: s => nodes[s], escHtml: s => s, fmtTs: s => s, Date,
    document: {createElement: () => ({replaceWith(img) { this.replacement = img; }})},
    fetch: url => new Promise(resolve => requests.push({url, resolve}))
  });
  vm.runInContext(code, ctx);
  return { ctx, nodes, requests };
}
const event = id => ({id, snapshot:'evt_' + id + '.jpg', snapshot_state:'available',
  event_type:'person_detected', confidence:0.9, ts:123, description:'event-' + id});
test('backward pagination renders current images, ignoring late next-page response', async () => {
  const {ctx, nodes, requests} = setup();
  const next = vm.runInContext('evPage=1; loadEvents()', ctx);
  const prev = vm.runInContext('evPage=0; loadEvents()', ctx);
  requests[1].resolve({ok:true, json:async()=>[event(100)]});
  await prev;
  requests[0].resolve({ok:true, json:async()=>[event(80)]});
  await next;
  assert.match(nodes['#ev-list'].innerHTML, /evt_100.jpg/);
  assert.doesNotMatch(nodes['#ev-list'].innerHTML, /evt_80.jpg/);
  assert.match(nodes['#ev-pager'].innerHTML, /第 1 页/);
});
test('leaving page prevents pending response from writing into a new view', async () => {
  const {ctx, nodes, requests} = setup();
  const pending = ctx.loadEvents();
  nodes['#ev-list'] = {innerHTML:'new view'};
  requests[0].resolve({ok:true, json:async()=>[event(80)]});
  await pending;
  assert.equal(nodes['#ev-list'].innerHTML, 'new view');
});
test('missing, absent, and available snapshots remain distinct', () => {
  const {ctx} = setup();
  assert.match(ctx.eventThumb({snapshot:''}), /无快照/);
  assert.match(ctx.eventThumb({...event(1),snapshot_state:'missing'}), /已清理或丢失/);
  assert.doesNotMatch(ctx.eventThumb({...event(1),snapshot_state:'missing'}), /<img/);
  assert.match(ctx.eventThumb(event(1)), /eventThumbError/);
});
test('failed image can be retried instead of permanently becoming no-image', () => {
  const {ctx} = setup();
  const img = {isConnected:true,src:'http://board/snap/evt_1.jpg',
    replaceWith(button) {this.button = button;}};
  ctx.eventThumbError(img);
  assert.match(img.button.textContent, /重试/);
  img.button.onclick();
  assert.match(img.src, /evt_1.jpg\?retry=/);
  assert.equal(img.button.replacement, img);
});
test('HTTP errors are shown as load failures', async () => {
  const {ctx, nodes, requests} = setup();
  const pending = ctx.loadEvents();
  requests[0].resolve({ok:false, status:503});
  await pending;
  assert.match(nodes['#ev-list'].innerHTML, /事件列表加载失败/);
});
