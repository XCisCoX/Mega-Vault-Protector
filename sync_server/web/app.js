/* MegaVault console. No framework, no storage: every value comes from the
   server on load, and the session lives in an httpOnly cookie. */
'use strict';

const $ = (id) => document.getElementById(id);

const state = {
  account: null,
  vaults: [],
  quota: { bytes: 0, limit: 0 },
  tokens: [],
  view: 'overview',
  protocol: 0,
  registration: 'open',
  busy: false,
};

/* ── formatting ──────────────────────────────────────────────────────── */

function fmtBytes(value) {
  const n = Number(value) || 0;
  if (n < 1024) return n + ' B';
  const units = ['KB', 'MB', 'GB', 'TB'];
  let size = n / 1024;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) { size /= 1024; unit += 1; }
  return size.toFixed(size >= 100 ? 0 : 1) + ' ' + units[unit];
}

function fmtDate(ms) {
  if (!ms) return '—';
  const date = new Date(Number(ms));
  if (Number.isNaN(date.getTime())) return '—';
  return date.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

function shortId(id) {
  return id ? String(id).slice(0, 10) + '…' : '—';
}

/* ── dom helpers ─────────────────────────────────────────────────────── */

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function clear(node) {
  while (node.firstChild) node.removeChild(node.firstChild);
}

function toast(message, isError) {
  const box = $('toast');
  box.textContent = message;
  box.className = 'toast' + (isError ? ' error' : '');
  box.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { box.hidden = true; }, isError ? 6000 : 3600);
}

async function copy(text, label) {
  try {
    await navigator.clipboard.writeText(text);
    toast((label || 'Copied') + ' to the clipboard');
  } catch (error) {
    toast('Copy failed — select the text and copy it manually', true);
  }
}

/* ── api ─────────────────────────────────────────────────────────────── */

async function api(method, path, body) {
  const options = { method, credentials: 'same-origin', headers: { Accept: 'application/json' } };
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  const text = await response.text();
  let value = null;
  if (text) {
    try { value = JSON.parse(text); } catch (error) { value = null; }
  }
  if (!response.ok) {
    const message = (value && value.error) || ('Request failed (' + response.status + ')');
    const failure = new Error(message);
    failure.status = response.status;
    throw failure;
  }
  return value || {};
}

/* ── modal ───────────────────────────────────────────────────────────── */

function ask(options) {
  return new Promise((resolve) => {
    const field = options.field || 'none';
    const box = $('modal');
    $('modal-title').textContent = options.title || 'Confirm';
    $('modal-text').textContent = options.text || '';
    $('modal-ok').textContent = options.confirm || 'Confirm';
    $('modal-field').hidden = field === 'none';
    $('modal-label').textContent = options.label || '';
    const input = $('modal-input');
    input.type = field === 'password' ? 'password' : 'text';
    input.value = options.value || '';
    input.placeholder = options.placeholder || '';
    box.hidden = false;
    if (field !== 'none') {
      input.focus();
      input.select();
    } else {
      $('modal-ok').focus();
    }

    const finish = (result) => {
      box.hidden = true;
      $('modal-ok').removeEventListener('click', ok);
      $('modal-cancel').removeEventListener('click', cancel);
      box.removeEventListener('keydown', keys);
      box.removeEventListener('click', outside);
      resolve(result);
    };
    const value = () => {
      if (field === 'none') return '';
      return input.value.trim();
    };
    const ok = () => finish(value());
    const cancel = () => finish(null);
    const keys = (event) => {
      if (event.key === 'Escape') { event.preventDefault(); cancel(); }
      if (event.key === 'Enter' && field !== 'none') { event.preventDefault(); ok(); }
    };
    const outside = (event) => { if (event.target === box) cancel(); };
    $('modal-ok').addEventListener('click', ok);
    $('modal-cancel').addEventListener('click', cancel);
    box.addEventListener('keydown', keys);
    box.addEventListener('click', outside);
  });
}

/* ── session ─────────────────────────────────────────────────────────── */

function showGate(mode) {
  $('app').hidden = true;
  $('auth').hidden = false;
  // A deployed relay may refuse self-service signup; never offer it then.
  const login = mode !== 'register' || state.registration !== 'open';
  $('login-form').hidden = !login;
  $('register-form').hidden = login;
  $('tab-login').classList.toggle('is-active', login);
  $('tab-register').classList.toggle('is-active', !login);
  $('tab-login').setAttribute('aria-selected', String(login));
  $('tab-register').setAttribute('aria-selected', String(!login));
  const focus = login ? $('login-form').querySelector('input[name=email]')
                      : $('register-form').querySelector('input[name=email]');
  if (focus) focus.focus();
}

function showApp() {
  $('auth').hidden = true;
  $('app').hidden = false;
  $('who').textContent = state.account.email;
  const since = el('span', null, 'Member since ' + fmtDate(state.account.created));
  $('who').appendChild(since);
  $('server-host').textContent = location.host;
}

async function loadAll() {
  const [session, vaults, tokens, health] = await Promise.all([
    api('GET', '/auth/session'),
    api('GET', '/v1/vaults'),
    api('GET', '/auth/token'),
    api('GET', '/healthz'),
  ]);
  state.account = session.account;
  state.vaults = vaults.vaults || [];
  state.quota = vaults.quota || { bytes: 0, limit: 0 };
  state.tokens = tokens.tokens || [];
  state.protocol = health.protocol || 0;
  showApp();
  render();
}

function signOutEverywhere() {
  state.account = null;
  state.vaults = [];
  state.tokens = [];
  showGate('login');
}

/* ── rendering ───────────────────────────────────────────────────────── */

const TITLES = {
  overview: ['Overview', 'Account, storage and devices at a glance.'],
  vaults: ['Vaults', 'A vault holds records and encrypted packages — nothing else.'],
  devices: ['Devices', 'One token per device, so a lost one can be revoked alone.'],
  account: ['Account', 'Password, sessions and the irreversible part.'],
  connect: ['Connect the app', 'Point a device at this server in three steps.'],
};

function setView(view) {
  state.view = view;
  for (const button of document.querySelectorAll('.nav-item')) {
    button.classList.toggle('is-active', button.dataset.view === view);
  }
  for (const section of document.querySelectorAll('.view')) {
    section.hidden = section.id !== 'view-' + view;
  }
  const [title, subtitle] = TITLES[view] || TITLES.overview;
  $('view-title').textContent = title;
  $('view-subtitle').textContent = subtitle;
  renderActions();
  render();
}

function renderActions() {
  const host = $('view-actions');
  clear(host);
  const action = (label, handler, primary) => {
    const button = el('button', 'button small' + (primary ? ' primary' : ''), label);
    button.type = 'button';
    button.addEventListener('click', handler);
    host.appendChild(button);
  };
  if (state.view === 'vaults') action('New vault', () => { setView('vaults'); $('vault-form').querySelector('input').focus(); }, true);
  if (state.view === 'devices') action('Issue token', () => { $('token-form').querySelector('input').focus(); }, true);
  if (state.view === 'connect') action('Copy address', () => copy(location.origin, 'Server address'));
  if (state.view === 'overview') action('Create vault', () => setView('vaults'), true);
}

function render() {
  if (!state.account) return;
  renderOverview();
  renderVaults();
  renderTokens();
  renderAccount();
  renderConnect();
}

function renderOverview() {
  const items = state.vaults.reduce((sum, vault) => sum + (vault.records || 0), 0);
  $('stat-vaults').textContent = String(state.vaults.length);
  $('stat-vaults-note').textContent = items + (items === 1 ? ' item' : ' items');
  $('stat-storage').textContent = fmtBytes(state.quota.bytes);
  $('stat-quota').textContent = 'of ' + fmtBytes(state.quota.limit);
  const active = state.tokens.filter((token) => !token.revoked);
  $('stat-devices').textContent = String(active.length);
  $('stat-devices-note').textContent = active.length ? 'active token' + (active.length === 1 ? '' : 's')
                                                     : 'none issued';
  $('stat-protocol').textContent = 'v' + state.protocol;
  $('stat-endpoint').textContent = location.host;

  const limit = Number(state.quota.limit) || 0;
  const used = Number(state.quota.bytes) || 0;
  const percent = limit > 0 ? Math.min(100, (used / limit) * 100) : 0;
  $('usage-fill').style.width = percent.toFixed(1) + '%';
  $('usage-meter').setAttribute('aria-valuenow', String(Math.round(percent)));
  $('usage-label').textContent = limit > 0
    ? Math.round(percent) + '% of ' + fmtBytes(limit) + ' used'
    : fmtBytes(used) + ' used';
  $('usage-hint').textContent = limit > 0
    ? fmtBytes(Math.max(0, limit - used)) + ' left. Deleting a vault reclaims its space immediately.'
    : 'No quota is configured on this server.';

  $('checklist').querySelector('[data-step=vault]').classList.toggle('done', state.vaults.length > 0);
  $('checklist').querySelector('[data-step=device]').classList.toggle('done', active.length > 0);
  $('checklist').querySelector('[data-step=connect]').classList.toggle('done', active.length > 0 && state.vaults.length > 0);
}

function renderVaults() {
  const body = $('vaults');
  clear(body);
  $('vaults-empty').hidden = state.vaults.length > 0;
  $('vault-table').hidden = state.vaults.length === 0;
  $('vaults-count').textContent = state.vaults.length
    ? state.vaults.length + (state.vaults.length === 1 ? ' vault' : ' vaults') : '';
  for (const vault of state.vaults) {
    const row = el('tr');
    row.appendChild(el('td', 'cell-name', vault.name));
    const id = el('td', 'mono', shortId(vault.id));
    id.title = vault.id;
    row.appendChild(id);
    row.appendChild(el('td', 'num', String(vault.records || 0)));
    row.appendChild(el('td', 'num', fmtBytes(vault.bytes)));
    row.appendChild(el('td', null, fmtDate(vault.created)));

    const actions = el('td', 'actions-col');
    const holder = el('div', 'actions');
    const copyId = el('button', 'button quiet small', 'Copy id');
    copyId.type = 'button';
    copyId.addEventListener('click', () => copy(vault.id, 'Vault id'));
    const rename = el('button', 'button quiet small', 'Rename');
    rename.type = 'button';
    rename.addEventListener('click', async () => {
      const name = await ask({
        title: 'Rename vault', text: 'The display name is stored on the server.',
        confirm: 'Rename', field: 'text', label: 'Vault name', value: vault.name,
      });
      if (!name || name === vault.name) return;
      await guard(async () => {
        await api('PATCH', '/v1/vaults/' + encodeURIComponent(vault.id), { name });
        toast('Vault renamed');
        await loadAll();
      });
    });
    const remove = el('button', 'button quiet small', 'Delete');
    remove.type = 'button';
    remove.addEventListener('click', async () => {
      const answer = await ask({
        title: 'Delete “' + vault.name + '”?',
        text: 'Its records and every encrypted package it holds are removed from ' +
              'this server. Local libraries keep their files, but nothing here ' +
              'can be recovered afterwards.',
        confirm: 'Delete vault',
      });
      if (answer === null) return;
      await guard(async () => {
        await api('DELETE', '/v1/vaults/' + encodeURIComponent(vault.id));
        toast('Vault deleted');
        await loadAll();
      });
    });
    holder.append(copyId, rename, remove);
    actions.appendChild(holder);
    row.appendChild(actions);
    body.appendChild(row);
  }
}

function renderTokens() {
  const body = $('tokens');
  clear(body);
  $('tokens-empty').hidden = state.tokens.length > 0;
  $('token-table').hidden = state.tokens.length === 0;
  const active = state.tokens.filter((token) => !token.revoked).length;
  $('tokens-count').textContent = state.tokens.length
    ? active + ' active of ' + state.tokens.length : '';
  for (const token of state.tokens) {
    const row = el('tr');
    row.appendChild(el('td', 'cell-name', token.label || 'device'));
    const id = el('td', 'mono', shortId(token.id));
    id.title = token.id;
    row.appendChild(id);
    row.appendChild(el('td', null, fmtDate(token.created)));
    row.appendChild(el('td', null)).appendChild(
      el('span', 'badge ' + (token.revoked ? '' : 'ok'), token.revoked ? 'revoked' : 'active'));

    const actions = el('td', 'actions-col');
    const holder = el('div', 'actions');
    if (!token.revoked) {
      const rename = el('button', 'button quiet small', 'Rename');
      rename.type = 'button';
      rename.addEventListener('click', async () => {
        const label = await ask({
          title: 'Rename device', text: 'Only the label changes; the token keeps working.',
          confirm: 'Rename', field: 'text', label: 'Device name', value: token.label,
        });
        if (!label || label === token.label) return;
        await guard(async () => {
          await api('PATCH', '/auth/token/' + encodeURIComponent(token.id), { label });
          toast('Device renamed');
          await loadAll();
        });
      });
      const revoke = el('button', 'button quiet small', 'Revoke');
      revoke.type = 'button';
      revoke.addEventListener('click', async () => {
        const answer = await ask({
          title: 'Revoke “' + (token.label || 'device') + '”?',
          text: 'That device loses access immediately. Issue a new token to reconnect it.',
          confirm: 'Revoke',
        });
        if (answer === null) return;
        await guard(async () => {
          await api('DELETE', '/auth/token/' + encodeURIComponent(token.id));
          toast('Device revoked');
          await loadAll();
        });
      });
      holder.append(rename, revoke);
    } else {
      holder.appendChild(el('span', 'hint', '—'));
    }
    actions.appendChild(holder);
    row.appendChild(actions);
    body.appendChild(row);
  }
}

function renderAccount() {
  $('account-email').textContent = state.account.email;
  $('account-id').textContent = state.account.id;
  $('account-created').textContent = fmtDate(state.account.created);
}

function renderConnect() {
  $('server-url').textContent = location.origin;
}

/* ── actions ─────────────────────────────────────────────────────────── */

async function guard(task) {
  if (state.busy) return;
  state.busy = true;
  try {
    await task();
  } catch (error) {
    if (error.status === 401) { signOutEverywhere(); return; }
    toast(error.message, true);
  } finally {
    state.busy = false;
  }
}

function bind() {
  $('tab-login').addEventListener('click', () => showGate('login'));
  $('tab-register').addEventListener('click', () => showGate('register'));
  $('switch-button').addEventListener('click', () => showGate('register'));

  $('login-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    guard(async () => {
      await api('POST', '/auth/login', {
        email: form.email.value.trim(),
        password: form.password.value,
      });
      form.reset();
      await loadAll();
      toast('Signed in');
    });
  });

  $('register-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    if (form.password.value !== form.confirm.value) {
      toast('The passwords do not match', true);
      return;
    }
    guard(async () => {
      await api('POST', '/auth/register', {
        email: form.email.value.trim(),
        password: form.password.value,
      });
      await api('POST', '/auth/login', {
        email: form.email.value.trim(),
        password: form.password.value,
      });
      form.reset();
      await loadAll();
      toast('Account created');
    });
  });

  $('signout').addEventListener('click', () => guard(async () => {
    await api('POST', '/auth/logout', {});
    signOutEverywhere();
    toast('Signed out');
  }));

  for (const button of document.querySelectorAll('.nav-item')) {
    button.addEventListener('click', () => setView(button.dataset.view));
  }

  $('vault-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const name = form.name.value.trim();
    if (!name) return;
    guard(async () => {
      await api('POST', '/v1/vaults', { name });
      form.reset();
      toast('Vault created');
      await loadAll();
    });
  });

  $('token-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const label = form.label.value.trim();
    if (!label) return;
    guard(async () => {
      const issued = await api('POST', '/auth/token', { label });
      form.reset();
      $('token-value').textContent = issued.token;
      $('new-token').hidden = false;
      $('copy-token').onclick = () => copy(issued.token, 'Token');
      toast('Token issued — copy it now');
      await loadAll();
    });
  });

  $('copy-url').addEventListener('click', () => copy(location.origin, 'Server address'));

  $('password-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    guard(async () => {
      const result = await api('POST', '/auth/password', {
        current_password: form.current.value,
        new_password: form.next.value,
      });
      form.reset();
      toast('Password updated' +
        (result.sessions_ended ? ' and ' + result.sessions_ended + ' other session(s) signed out' : ''));
    });
  });

  $('signout-all').addEventListener('click', async () => {
    const answer = await ask({
      title: 'Sign out everywhere?',
      text: 'Every browser session for this account ends, including this one.',
      confirm: 'Sign out everywhere',
    });
    if (answer === null) return;
    await guard(async () => {
      await api('DELETE', '/auth/sessions');
      signOutEverywhere();
      toast('Signed out everywhere');
    });
  });

  $('delete-account').addEventListener('click', async () => {
    const password = await ask({
      title: 'Delete this account?',
      text: 'The account, its vaults, every record and every encrypted package on ' +
            'this server are removed. Nothing can be recovered. Type the account ' +
            'password to confirm.',
      confirm: 'Delete everything', field: 'password', label: 'Account password',
    });
    if (password === null) return;
    if (!password) { toast('The password is required', true); return; }
    await guard(async () => {
      const result = await api('DELETE', '/auth/account', { password });
      signOutEverywhere();
      toast('Account deleted — ' + (result.vaults_removed || 0) + ' vault(s) removed');
    });
  });
}

/* ── start ───────────────────────────────────────────────────────────── */

async function start() {
  bind();
  try {
    // The public health probe says whether this host accepts new accounts.
    const health = await api('GET', '/healthz');
    state.registration = health.registration || 'open';
    if (state.registration !== 'open') {
      $('tab-register').hidden = true;
      const hint = $('switch-button').closest('p');
      if (hint) hint.hidden = true;
    }
    await loadAll();
    setView('overview');
  } catch (error) {
    if (error.status === 401) { showGate('login'); return; }
    showGate('login');
    toast(error.message || 'Cannot reach the server', true);
  }
}

start();
