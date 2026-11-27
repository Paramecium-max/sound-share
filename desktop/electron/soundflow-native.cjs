const { spawn } = require('node:child_process');
const readline = require('node:readline');

class SoundFlowNative {
  constructor(binary, log) {
    this.log = log;
    this.pending = new Map();
    this.sequence = 0;
    this.child = spawn(binary, [], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    this.ready = new Promise((resolve, reject) => {
      this.resolveReady = resolve;
      this.rejectReady = reject;
    });
    this.ready.catch(() => {});
    this.startup = setTimeout(() => {
      this.fail(new Error('等待音流原生内核启动超时'));
      this.child.kill();
    }, 8000);
    readline.createInterface({ input: this.child.stdout }).on('line', (line) => {
      let message;
      try { message = JSON.parse(line); } catch { this.log(line); return; }
      if (message.ready === true) {
        clearTimeout(this.startup);
        this.resolveReady();
        return;
      }
      this.pending.get(message.id)?.finish(message.error, message.result);
    });
    this.child.stderr.setEncoding('utf8').on('data', (data) => this.log(data));
    this.child.stdin.on('error', (error) => this.fail(error));
    this.child.on('error', (error) => this.fail(error));
    this.child.on('close', (code) => this.fail(new Error(`音流原生内核已退出 (${code})`)));
  }

  fail(error) {
    if (this.failure) return;
    this.failure = error;
    clearTimeout(this.startup);
    this.rejectReady(error);
    for (const pending of this.pending.values()) pending.finish(error.message);
    this.log(error.message);
  }

  async request(method, args = {}) {
    await this.ready;
    if (this.failure) throw this.failure;
    if (this.pending.size >= 64) throw new Error('音流原生内核繁忙');
    const id = ++this.sequence;
    return new Promise((resolve, reject) => {
      const finish = (error, result) => {
        clearTimeout(timer);
        this.pending.delete(id);
        if (error) reject(new Error(error));
        else resolve(result);
      };
      const timer = setTimeout(() => finish(`原生内核请求超时 (${method})`), 7500);
      this.pending.set(id, { finish });
      this.child.stdin.write(JSON.stringify({ id, method, args }) + '\n', (error) => {
        if (error) finish(error.message);
      });
    });
  }

  close() {
    this.child.stdin.end();
    const timer = setTimeout(() => this.child.kill(), 3000);
    timer.unref();
    this.child.once('close', () => clearTimeout(timer));
  }
}

module.exports = { SoundFlowNative };
