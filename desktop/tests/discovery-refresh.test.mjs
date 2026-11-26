import assert from 'node:assert/strict';
import test from 'node:test';
import { allowAutomaticRefresh } from '../src/discovery-refresh.mjs';

test('automatic refresh combines opening, focus, visibility and the devices tab', () => {
  let last;
  const socket = {};
  const trigger = (now, current = socket) => {
    if (!allowAutomaticRefresh(last, current, now)) return false;
    last = { socket: current, at: now };
    return true;
  };
  assert.equal(trigger(0), true);      // connected
  assert.equal(trigger(1), false);     // device tab
  assert.equal(trigger(18), false);    // focus
  assert.equal(trigger(44), false);    // visible
  assert.equal(trigger(1500), true);  // return to foreground
  assert.equal(trigger(1550), false);  // focus
  assert.equal(trigger(1580), false);  // device tab
  assert.equal(trigger(1620, {}), true); // reconnect
});
