'use strict';

const $ = (id) => document.getElementById(id);
let quota = {bytes: 0, limit: 1};

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function size(count) {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let value = Number(count) || 0;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return (unit === 0 ? value : value.toFixed(1)) + ' ' + units[unit];
}

function when(ms) {
  return new Date(Number(ms)).toLocaleDateString(undefined, {year: 'numeric', month: 'short', day: 'numeric'});
}

let toastTimer = null;
function toast(message) {
  const box = $('toast');
  box.textContent = message;
  box.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { box.hidden = true; }, 4000);
}

async function api(method, path, body) {
  const options = {method, credentials: 'same-origin', headers: {}};
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  const text = await response.text();
  let data = null;
  if (text) {
    try { data = JSON.parse(text); } catch { data = null; }
  }
  if (!response.ok) {
    if (response.status === 401) signedOut();
    const error = new Error((data && data.error) || 'Request failed (' + response.status + ')');
    error.status = response.status;
    throw error;
  }
  return data;
}

function signedIn(account) {
  $('auth').hidden = true;
  $('app').hidden = false;
  $('signout').hidden = false;
  $('who').textContent = account.email;
  $('server-url').textContent = location.origin;
  refresh();
}

function signedOut() {
  $('auth').hidden = false;
  $('app').hidden = true;
  $('signout').hidden = true;
  $('who').textContent = '';
}

async function refresh() {
  try {
    const [data, tokens] = await Promise.all([api('GET', '/v1/vaults'), api('GET', '/auth/token')]);
    quota = data.quota;
    drawUsage();
    drawVaults(data.vaults);
    drawTokens(tokens.tokens);
  } catch (error) {
    if (error.status !== 401) toast(error.message);
  }
}

function drawUsage() {
  $('usage').textContent = size(quota.bytes) + ' of ' + size(quota.limit) + ' used';
  const share = quota.limit ? Math.min(100, (quota.bytes / quota.limit) * 100) : 0;
  $('meter').style.width = share.toFixed(1) + '%';
  $('meter').className = share > 90 ? 'full' : '';
}

function drawVaults(vaults) {
  const list = $('vaults');
  list.replaceChildren();
  $('vaults-empty').hidden = vaults.length > 0;
  for (const vault of vaults) {
    const row = el('li');
    const text = el('div', 'grow');
    text.append(el('strong', null, vault.name));
    text.append(el('span', 'muted small',
      vault.records + (vault.records === 1 ? ' item · ' : ' items · ') + size(vault.bytes) +
      ' · created ' + when(vault.created)));
    const rename = el('button', 'link', 'Rename');
    rename.type = 'button';
    rename.addEventListener('click', async () => {
      const name = prompt('New name for this vault', vault.name);
      if (!name || name.trim() === vault.name) return;
      try {
        await api('PATCH', '/v1/vaults/' + vault.id, {name: name.trim()});
        refresh();
      } catch (error) { toast(error.message); }
    });
    const remove = el('button', 'link danger', 'Delete');
    remove.type = 'button';
    remove.addEventListener('click', async () => {
      if (!confirm('Delete the vault "' + vault.name + '" and every package stored in it? Devices keep their local copies.')) return;
      try {
        await api('DELETE', '/v1/vaults/' + vault.id);
        refresh();
      } catch (error) { toast(error.message); }
    });
    row.append(text, rename, remove);
    list.append(row);
  }
}

function drawTokens(tokens) {
  const list = $('tokens');
  list.replaceChildren();
  if (!tokens.length) {
    list.append(el('li', 'muted small', 'No devices yet.'));
    return;
  }
  for (const token of tokens) {
    const row = el('li');
    const text = el('div', 'grow');
    text.append(el('strong', null, token.label));
    text.append(el('span', 'muted small',
      (token.revoked ? 'revoked' : 'issued') + ' ' + when(token.created)));
    row.append(text);
    if (!token.revoked) {
      const button = el('button', 'link danger', 'Revoke');
      button.type = 'button';
      button.addEventListener('click', async () => {
        try {
          await api('DELETE', '/auth/token/' + token.id);
          refresh();
        } catch (error) { toast(error.message); }
      });
      row.append(button);
    } else {
      row.append(el('span', 'tag', 'revoked'));
    }
    list.append(row);
  }
}

function swapAuth(register) {
  $('login-form').hidden = register;
  $('register-form').hidden = !register;
  $('auth-title').textContent = register ? 'Create an account' : 'Sign in';
  $('switch').textContent = register ? 'Already registered? Sign in' : 'No account? Create one';
}

async function submit(form, path) {
  const data = Object.fromEntries(new FormData(form).entries());
  try {
    await api('POST', path, data);
    form.reset();
    const session = await api('GET', '/auth/session');
    signedIn(session.account);
  } catch (error) {
    toast(error.message);
  }
}

$('login-form').addEventListener('submit', (event) => {
  event.preventDefault();
  submit(event.target, '/auth/login');
});

$('register-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = event.target;
  const data = Object.fromEntries(new FormData(form).entries());
  try {
    await api('POST', '/auth/register', data);
    await api('POST', '/auth/login', data);
    form.reset();
    signedIn((await api('GET', '/auth/session')).account);
  } catch (error) {
    toast(error.message);
  }
});

$('switch').addEventListener('click', () => swapAuth($('login-form').hidden === false));

$('signout').addEventListener('click', async () => {
  try { await api('POST', '/auth/logout', {}); } catch (error) { /* already out */ }
  signedOut();
});

$('vault-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = event.target;
  const name = new FormData(form).get('name');
  try {
    await api('POST', '/v1/vaults', {name});
    form.reset();
    refresh();
  } catch (error) { toast(error.message); }
});

$('token-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = event.target;
  const label = new FormData(form).get('label') || 'device';
  try {
    const issued = await api('POST', '/auth/token', {label});
    const box = $('new-token');
    box.textContent = issued.token;
    box.hidden = false;
    form.reset();
    refresh();
  } catch (error) { toast(error.message); }
});

(async () => {
  try {
    signedIn((await api('GET', '/auth/session')).account);
  } catch (error) {
    signedOut();
  }
})();
