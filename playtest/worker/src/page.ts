/**
 * A tester's sessions and their reviews. The launcher opens it as `/#token=TOKEN`; a fragment never
 * reaches the server, and the page keeps the token in the browser for later visits.
 */
export const PAGE = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Play sessions</title>
<style>
:root { --bg: #f7f7f5; --card: #fff; --text: #1d1d1b; --muted: #6b6b66; --line: #e2e2dd; --accent: #2f6fdb; --good: #237a3b; --wait: #8a6d1d; }
@media (prefers-color-scheme: dark) { :root { --bg: #161615; --card: #1f1f1d; --text: #ececea; --muted: #9a9a94; --line: #33332f; --accent: #7aa7ff; --good: #6cc486; --wait: #d9b85c; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.5 system-ui, sans-serif; }
main { max-width: 760px; margin: 0 auto; padding: 32px 16px; }
h1 { font-size: 22px; margin: 0 0 4px; }
.sub { color: var(--muted); margin: 0 0 24px; }
.session { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: 16px; margin-bottom: 12px; }
.head { display: flex; flex-wrap: wrap; justify-content: space-between; gap: 8px; }
.when { font-weight: 600; }
.meta { color: var(--muted); font-size: 13px; }
.status { font-size: 13px; font-weight: 600; }
.status.reviewed { color: var(--good); }
.status.playing, .status.ended { color: var(--wait); }
.found { margin: 10px 0 0; font-weight: 600; }
.summary { margin: 8px 0 0; white-space: pre-wrap; }
code { font-size: 13px; }
.empty, .error { color: var(--muted); }
</style>
</head>
<body>
<main>
<h1>Play sessions</h1>
<p class="sub" id="who">Loading…</p>
<div id="list"></div>
</main>
<script>
const STATUS = { playing: 'Playing or uploading', ended: 'Waiting for review', reviewed: 'Reviewed' }
const store = { get() { try { return localStorage.getItem('playtest-token') } catch { return null } },
                set(v) { try { localStorage.setItem('playtest-token', v) } catch {} } }
const fromHash = new URLSearchParams(location.hash.slice(1)).get('token')
if (fromHash) { store.set(fromHash); history.replaceState(null, '', location.pathname) }
const token = fromHash || store.get()
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e }
const minutes = (vblanks) => Math.round(vblanks / 3600)

function card(s) {
  const c = el('div', 'session')
  const head = el('div', 'head')
  const left = el('div')
  left.append(el('div', 'when', new Date(s.started_at).toLocaleString()))
  left.append(el('div', 'meta', s.product + ' · build ' + s.build + ' · ' + minutes(s.vblanks) + ' min played' + (s.exit ? ' · ' + s.exit : '')))
  head.append(left, el('div', 'status ' + s.status, STATUS[s.status]))
  c.append(head)
  if (s.status !== 'reviewed') return c
  const missed = JSON.parse(s.missed || '[]')
  const found = [s.new_functions ? s.new_functions + (s.new_functions === 1 ? ' function' : ' functions') + ' the recompiler had missed' : 'No missed functions',
                 s.new_bytes.toLocaleString() + ' bytes of code run for the first time by anyone']
  c.append(el('p', 'found', found.join(' · ')))
  if (missed.length) { const p = el('p', 'meta', 'Missed functions: '); p.append(el('code', null, missed.join(', '))); c.append(p) }
  c.append(el('p', 'summary', s.summary))
  return c
}

async function load() {
  const who = document.getElementById('who'), list = document.getElementById('list')
  if (!token) { who.textContent = 'Open this page from the launcher, which adds your tester link.'; return }
  const r = await fetch('/api/me', { headers: { authorization: 'Bearer ' + token } })
  if (!r.ok) { who.textContent = 'This tester link is not valid any more. Ask for a new one.'; who.className = 'error'; return }
  const me = await r.json()
  who.textContent = me.tester + ' · ' + me.sessions.length + (me.sessions.length === 1 ? ' session' : ' sessions')
  if (!me.sessions.length) list.append(el('p', 'empty', 'No sessions yet. Each one shows up here as soon as you start playing.'))
  for (const s of me.sessions) list.append(card(s))
}
load().catch(() => { document.getElementById('who').textContent = 'Could not reach the server.' })
</script>
</body>
</html>
`
