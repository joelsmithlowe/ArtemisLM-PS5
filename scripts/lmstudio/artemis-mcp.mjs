#!/usr/bin/env node
// LM Studio launches this on the computer. It speaks MCP on stdin and calls
// ArtemisLM on the PlayStation. Chat is the OpenAI API on port 47281. The
// library is port 47282. One model stays on the GPU.
//
//   ARTEMIS_HOST=192.168.1.19 node scripts/lmstudio/artemis-mcp.mjs
//
// Works with a console on 0.10.0. A later build also serves MCP itself at
// http://<PS5>:47282/mcp, which LM Studio can use without this script.
// artemis_reference calls POST /reference, so that one tool needs 0.12.0 or later.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import http from 'node:http';

const host = process.env.ARTEMIS_HOST || '';
const chatPort = Number(process.env.ARTEMIS_CHAT_PORT || 47281);
const libraryPort = Number(process.env.ARTEMIS_LIBRARY_PORT || 47282);

function hostOk(value) {
  return /^[A-Za-z0-9.-]{1,253}$/.test(value) && value.indexOf('..') < 0;
}

function portOk(value) {
  return Number.isInteger(value) && value > 0 && value < 65536;
}

function call(port, method, path, body, timeoutMs) {
  return new Promise((resolve) => {
    if (!hostOk(host) || !portOk(port)) {
      resolve({ status: 0, json: null, error: 'Set ARTEMIS_HOST to the PlayStation address.' });
      return;
    }
    const payload = body ? Buffer.from(JSON.stringify(body)) : null;
    const req = http.request({
      host,
      port,
      method,
      path,
      headers: payload ? { 'Content-Type': 'application/json', 'Content-Length': payload.length } : {},
      timeout: timeoutMs
    }, (res) => {
      const chunks = [];
      res.on('data', (chunk) => chunks.push(chunk));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = text ? JSON.parse(text) : null; } catch (e) { json = null; }
        resolve({ status: res.statusCode || 0, json, error: '' });
      });
    });
    req.on('timeout', () => {
      req.destroy();
      resolve({ status: 0, json: null, error: 'The PlayStation did not answer in time.' });
    });
    req.on('error', () => resolve({ status: 0, json: null, error: 'The PlayStation did not answer.' }));
    if (payload) req.write(payload);
    req.end();
  });
}

function contentOf(message) {
  if (!message) return '';
  const content = message.content;
  if (typeof content === 'string') return content;
  if (Array.isArray(content)) {
    return content.map((part) => {
      if (typeof part === 'string') return part;
      if (part && part.type === 'text' && typeof part.text === 'string') return part.text;
      return '';
    }).join('');
  }
  if (typeof message.reasoning_content === 'string') return message.reasoning_content;
  return '';
}

function answerOf(got) {
  const choice = got.json && got.json.choices && got.json.choices[0] && got.json.choices[0].message;
  const text = contentOf(choice);
  if (text) return { text, error: '' };
  const err = got.json && got.json.error;
  if (typeof err === 'string') return { text: '', error: err };
  if (err && typeof err.message === 'string') return { text: '', error: err.message };
  return { text: '', error: got.error || 'The model did not answer.' };
}

const tools = [
  {
    name: 'artemis_status',
    description: 'Read ArtemisLM on the PlayStation. Reports the version, the loaded model, agent mode, and the chat address. One model stays on the GPU.',
    inputSchema: { type: 'object', properties: {} }
  },
  {
    name: 'artemis_models',
    description: 'List the models on the PlayStation. Each line has the name, whether it is loaded, whether it fits, the context, and the file path.',
    inputSchema: { type: 'object', properties: {} }
  },
  {
    name: 'artemis_ask',
    description: 'Ask the model loaded on the PlayStation. One model stays on the GPU. plan true writes a short plan first, then the answer, on that same model. This can take a few minutes.',
    inputSchema: {
      type: 'object',
      properties: {
        prompt: { type: 'string', description: 'The question. Used when messages is empty.' },
        system: { type: 'string', description: 'Optional note placed before the question.' },
        messages: { type: 'array', description: 'OpenAI chat messages. Used instead of prompt when present.' },
        max_tokens: { type: 'integer', description: 'Cap on the answer. Omit it to let the loaded model decide.' },
        plan: { type: 'boolean', description: 'Write a short plan first. Omit it to follow Agent mode on the console.' }
      }
    }
  },
  {
    name: 'artemis_load',
    description: 'Restart the PlayStation app onto a model file from artemis_models. One model stays on the GPU. After this, wait and call artemis_status before the next ask.',
    inputSchema: {
      type: 'object',
      properties: { path: { type: 'string', description: 'Absolute path of a .gguf from artemis_models.' } },
      required: ['path']
    }
  },
  {
    name: 'artemis_reference',
    description: 'Look up a small table stored on the PlayStation. No download. tool is convert, calendar, lookup, text, or note. convert needs value, from, and to. calendar op is weekday, between, or add. lookup kind is constant, element, http, or prefix. text op is base64, unbase64, hex, sha256, or count.',
    inputSchema: {
      type: 'object',
      properties: {
        tool: { type: 'string', description: 'convert, calendar, lookup, text, or note.' },
        value: { type: 'number', description: 'The number to convert.' },
        from: { type: 'string', description: 'Unit to convert from, such as km or celsius.' },
        to: { type: 'string', description: 'Unit to convert to, such as miles or fahrenheit.' },
        op: { type: 'string', description: 'calendar: weekday, between, or add. text: base64, unbase64, hex, sha256, or count.' },
        date: { type: 'string', description: 'YYYY-MM-DD, or a date such as 18 October 2026.' },
        end: { type: 'string', description: 'The second date for calendar between.' },
        days: { type: 'integer', description: 'Days to add. Negative goes backward.' },
        kind: { type: 'string', description: 'lookup kind: constant, element, http, or prefix.' },
        name: { type: 'string', description: 'Constant, element, or SI prefix. pi, iron, Fe, or nano.' },
        code: { type: 'integer', description: 'HTTP status code, such as 404.' },
        text: { type: 'string', description: 'Text to encode, or the question for tool note.' }
      },
      required: ['tool']
    }
  }
];

async function statusText() {
  const got = await call(libraryPort, 'GET', '/artemis', null, 10000);
  if (!got.json) return { text: '', error: got.error || 'The PlayStation did not answer.' };
  const loaded = typeof got.json.loaded === 'string' ? got.json.loaded : 'none';
  const lines = [
    (got.json.name || 'ArtemisLM PS5') + ' ' + (got.json.version || ''),
    'Loaded: ' + loaded,
    'Chat: ' + (got.json.chat || ('http://' + host + ':' + chatPort + '/v1')),
    'One model stays on the GPU. Any model name works on the chat port.'
  ];
  return { text: lines.join('\n'), error: '' };
}

async function modelsText() {
  const got = await call(libraryPort, 'GET', '/v1/models', null, 20000);
  const data = got.json && Array.isArray(got.json.data) ? got.json.data : null;
  if (!data) return { text: '', error: got.error || 'The library did not answer.' };
  if (!data.length) return { text: 'No models in the library.', error: '' };
  const lines = data.map((model) => {
    const extra = model.artemislm || {};
    const bits = [model.id || 'model'];
    if (extra.loaded) bits.push('loaded');
    bits.push(extra.fits ? 'fits' : 'does not fit');
    if (model.context_length) bits.push('ctx ' + model.context_length);
    if (extra.path) bits.push(extra.path);
    return bits.join('  ');
  });
  return { text: lines.join('\n'), error: '' };
}

function messagesFrom(args) {
  const messages = [];
  if (Array.isArray(args.messages)) {
    for (const message of args.messages) {
      if (!message || typeof message.role !== 'string') continue;
      const text = contentOf(message);
      if (!text) continue;
      if (message.role !== 'system' && message.role !== 'user' && message.role !== 'assistant' && message.role !== 'tool') continue;
      messages.push({ role: message.role, content: text });
    }
  }
  if (!messages.length) {
    if (typeof args.system === 'string' && args.system) messages.push({ role: 'system', content: args.system });
    if (typeof args.prompt === 'string' && args.prompt) messages.push({ role: 'user', content: args.prompt });
  } else if (typeof args.system === 'string' && args.system) {
    if (messages[0].role === 'system') messages[0].content += '\n' + args.system;
    else messages.unshift({ role: 'system', content: args.system });
  }
  let user = '';
  for (const message of messages) if (message.role === 'user') user = message.content;
  if (!user && typeof args.prompt === 'string' && args.prompt) {
    messages.push({ role: 'user', content: args.prompt });
    user = args.prompt;
  }
  return { messages, user };
}

async function referenceText(args) {
  const got = await call(libraryPort, 'POST', '/reference', args || {}, 10000);
  const err = got.json && typeof got.json.error === 'string' ? got.json.error : '';
  if (err) return { text: '', error: err };
  if (got.json && typeof got.json.text === 'string') return { text: got.json.text, error: '' };
  return { text: '', error: got.error || 'The console has no reference table.' };
}

async function askText(args) {
  const built = messagesFrom(args || {});
  if (!built.messages.length || !built.user) return { text: '', error: 'Say what to ask.' };
  const noted = await referenceText({ tool: 'note', text: built.user });
  if (noted.text && noted.text.indexOf('Nothing in this table') !== 0) {
    const line = '\n\nReference:\n' + noted.text + '\nUse this figure. Do not replace it.';
    if (built.messages[0].role === 'system') built.messages[0].content += line;
    else built.messages.unshift({ role: 'system', content: 'You are ArtemisLM PS5.' + line });
  }
  let wantPlan = args.plan === true;
  let planTokens = 256;
  let agent = 'off';
  if (typeof args.plan !== 'boolean') {
    const settings = await call(libraryPort, 'GET', '/settings', null, 10000);
    if (settings.json && (settings.json.agent === 'plan' || settings.json.agent === 'handoff')) {
      wantPlan = true;
      agent = settings.json.agent;
    }
    if (settings.json && (settings.json.plan_tokens === 128 || settings.json.plan_tokens === 512)) {
      planTokens = settings.json.plan_tokens;
    }
  }
  let plan = '';
  if (wantPlan) {
    const planned = await call(chatPort, 'POST', '/v1/chat/completions', {
      model: 'artemis',
      stream: false,
      max_tokens: planTokens,
      chat_template_kwargs: { enable_thinking: false },
      messages: [
        { role: 'system', content: 'You are the coordinator on ArtemisLM PS5. Write a short plan for the answer. At most 8 lines. Do not answer the question. One model is loaded.' },
        { role: 'user', content: 'Plan this, do not answer:\n' + built.user }
      ]
    }, 120000);
    plan = answerOf(planned).text;
    if (plan.length > 4000) plan = plan.slice(0, 4000);
    if (plan) {
      const line = '\n\nCoordinator plan:\n' + plan + '\nFollow the plan. This console keeps one model loaded.';
      if (built.messages[0].role === 'system') built.messages[0].content += line;
      else built.messages.unshift({ role: 'system', content: 'You are ArtemisLM PS5.' + line });
    }
  }
  let maxTokens = 0;
  if (Number.isInteger(args.max_tokens)) {
    maxTokens = args.max_tokens;
    if (maxTokens < 1) maxTokens = 1;
    if (maxTokens > 8192) maxTokens = 8192;
  }
  const payload = {
    model: 'artemis',
    stream: false,
    messages: built.messages,
    chat_template_kwargs: { enable_thinking: false }
  };
  if (maxTokens) payload.max_tokens = maxTokens;
  const got = await call(chatPort, 'POST', '/v1/chat/completions', payload, 300000);
  const answer = answerOf(got);
  if (!answer.text) return { text: '', error: answer.error };
  let text = '';
  if (plan) text += 'Plan:\n' + plan + '\n\n';
  text += answer.text;
  if (agent === 'handoff') text += '\n\nHand off restarts the app from the phone page. This answer stays on the loaded model.';
  if (text.length > 24000) text = text.slice(0, 24000) + '\nThe reply was cut.';
  return { text, error: '' };
}

async function loadText(args) {
  const path = args && typeof args.path === 'string' ? args.path : '';
  if (!path) return { text: '', error: 'Name the model file from artemis_models.' };
  const got = await call(libraryPort, 'POST', '/load', { path }, 15000);
  const err = got.json && typeof got.json.error === 'string' ? got.json.error : '';
  if (err === 'That model is already loaded') return { text: err, error: '' };
  if (err) return { text: '', error: err };
  if (got.json && typeof got.json.note === 'string') return { text: got.json.note, error: '' };
  return { text: '', error: got.error || 'The console could not start the load.' };
}

async function runTool(name, args) {
  if (name === 'artemis_status') return statusText();
  if (name === 'artemis_models') return modelsText();
  if (name === 'artemis_ask') return askText(args || {});
  if (name === 'artemis_load') return loadText(args || {});
  if (name === 'artemis_reference') return referenceText(args || {});
  return { text: '', error: 'That tool is not on this console.' };
}

function resultFor(id, result) {
  return { jsonrpc: '2.0', id, result };
}

async function handle(msg) {
  if (!msg || msg.jsonrpc !== '2.0' || typeof msg.method !== 'string') {
    return { jsonrpc: '2.0', id: msg && msg.id, error: { code: -32700, message: 'The request was not JSON-RPC.' } };
  }
  const hasId = Object.prototype.hasOwnProperty.call(msg, 'id') && msg.id !== null && msg.id !== undefined;
  if (!hasId || msg.method.indexOf('notifications/') === 0) return null;
  const params = msg.params && typeof msg.params === 'object' ? msg.params : {};
  if (msg.method === 'initialize') {
    const version = typeof params.protocolVersion === 'string' && params.protocolVersion.length < 33
      ? params.protocolVersion : '2025-03-26';
    return resultFor(msg.id, {
      protocolVersion: version,
      capabilities: { tools: {} },
      serverInfo: { name: 'ArtemisLM PS5', version: '1' }
    });
  }
  if (msg.method === 'ping') return resultFor(msg.id, {});
  if (msg.method === 'tools/list') return resultFor(msg.id, { tools });
  if (msg.method === 'tools/call') {
    const ran = await runTool(params.name, params.arguments || {});
    const text = ran.text || ran.error || 'The console could not run that.';
    return resultFor(msg.id, {
      content: [{ type: 'text', text: text.length > 24000 ? text.slice(0, 24000) + '\nThe reply was cut.' : text }],
      isError: !ran.text
    });
  }
  return { jsonrpc: '2.0', id: msg.id, error: { code: -32601, message: 'That method is not on this console.' } };
}

function writeMessage(obj) {
  if (!obj) return;
  const body = Buffer.from(JSON.stringify(obj));
  process.stdout.write('Content-Length: ' + body.length + '\r\n\r\n');
  process.stdout.write(body);
}

let buffer = Buffer.alloc(0);
let pumping = Promise.resolve();

function takeString(text, end) {
  let escaped = false;
  for (let i = 1; i < text.length; i++) {
    const ch = text[i];
    if (escaped) escaped = false;
    else if (ch === '\\') escaped = true;
    else if (ch === '"') return i + 1;
  }
  return -1;
}

function takeObject(text) {
  if (!text.startsWith('{')) return -1;
  let depth = 0;
  let i = 0;
  while (i < text.length) {
    const ch = text[i];
    if (ch === '"') {
      const next = takeString(text.slice(i), 0);
      if (next < 0) return -1;
      i += next;
      continue;
    }
    if (ch === '{') depth++;
    else if (ch === '}') {
      depth--;
      if (depth === 0) return i + 1;
    }
    i++;
  }
  return -1;
}

async function drain() {
  for (;;) {
    let start = 0;
    while (start < buffer.length && (buffer[start] === 32 || buffer[start] === 10 || buffer[start] === 13 || buffer[start] === 9)) start++;
    if (start) buffer = buffer.subarray(start);
    if (!buffer.length) return;
    if (buffer[0] === 67 || buffer[0] === 99) {
      const text = buffer.toString('utf8');
      const head = text.indexOf('\r\n\r\n');
      if (head < 0) return;
      const match = /content-length:\s*(\d+)/i.exec(text.slice(0, head));
      if (!match) {
        buffer = Buffer.from(text.slice(head + 4));
        continue;
      }
      const length = Number(match[1]);
      const bodyStart = Buffer.byteLength(text.slice(0, head + 4));
      if (buffer.length < bodyStart + length) return;
      const json = buffer.subarray(bodyStart, bodyStart + length).toString('utf8');
      buffer = buffer.subarray(bodyStart + length);
      try { writeMessage(await handle(JSON.parse(json))); } catch (e) {
        writeMessage({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'The request was not JSON-RPC.' } });
      }
      continue;
    }
    if (buffer[0] === 123) {
      const text = buffer.toString('utf8');
      const end = takeObject(text);
      if (end < 0) return;
      const json = text.slice(0, end);
      buffer = Buffer.from(text.slice(end));
      try { writeMessage(await handle(JSON.parse(json))); } catch (e) {
        writeMessage({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'The request was not JSON-RPC.' } });
      }
      continue;
    }
    const newline = buffer.indexOf(10);
    if (newline < 0) return;
    buffer = buffer.subarray(newline + 1);
  }
}

process.stdin.on('data', (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  pumping = pumping.then(drain).catch((err) => {
    process.stderr.write(String(err && err.message ? err.message : err) + '\n');
  });
});
process.stdin.on('end', () => process.exit(0));
