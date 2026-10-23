const pending = new Map();
const listeners = new Map();
let nextId = 1;
let bound = false;

function bridge() {
    return window.chrome && window.chrome.webview;
}

function ensure() {
    if (bound) return;
    bound = true;
    const host = bridge();
    if (host) host.addEventListener('message', onMessage);
}

function onMessage(event) {
    const message = event.data;
    if (!message || typeof message !== 'object') return;

    if (message.event) {
        const set = listeners.get(message.event);
        if (set) {
            for (const fn of [...set]) {
                try {
                    fn(message.data || {}, message);
                } catch (error) {
                    console.error(`[rpc] ${message.event} listener failed`, error);
                }
            }
        }
        return;
    }

    const entry = pending.get(message.id);
    if (!entry) return;
    pending.delete(message.id);
    if (message.error) entry.reject(message.error);
    else entry.resolve(message.result);
}

export function call(method, params = {}) {
    ensure();
    const host = bridge();
    if (!host) {
        return Promise.reject({
            message: 'WebView2 桥不可用',
            context: '请通过 cmcl-ui.exe 启动本界面。',
        });
    }
    return new Promise((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve, reject });
        host.postMessage({ id, method, params });
    });
}

export function onEvent(name, fn) {
    ensure();
    let set = listeners.get(name);
    if (!set) {
        set = new Set();
        listeners.set(name, set);
    }
    set.add(fn);
    return () => {
        set.delete(fn);
        if (!set.size) listeners.delete(name);
    };
}

export function bridgeAvailable() {
    return Boolean(bridge());
}
