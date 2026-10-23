import { call, onEvent } from './rpc.js';

const LINE_LIMIT = 4000;

const stateSubscribers = new Set();
const lineSubscribers = new Set();

let initialised = false;

export const gameStore = {
    running: false,
    launching: false,
    pid: null,
    version: null,
    exitCode: null,
    manualStop: false,
    summary: null,
    lines: [],

    init() {
        if (initialised) return;
        initialised = true;

        onEvent('game.log', (data) => {
            this.push(String(data?.line ?? ''));
        });

        onEvent('game.exit', (data) => {
            this.running = false;
            this.launching = false;
            this.pid = null;
            this.version = null;
            this.exitCode = data?.code ?? null;
            this.manualStop = Boolean(data?.manual);
            this.push(this.manualStop
                ? '[launcher] 游戏已停止'
                : `[launcher] 游戏已退出，退出码 ${this.exitCode}`);
            this.emit();
        });

        onEvent('game.summary', (data) => {
            this.summary = data || null;
            this.emit();
        });

        this.sync();
    },

    async sync() {
        try {
            const status = await call('game.status');
            this.running = Boolean(status?.running);
            this.pid = status?.pid ?? null;
            this.emit();
        } catch (_) {
            // 桥未就绪时忽略，页面会在启动时再次查询。
        }
    },

    begin(version = null) {
        this.launching = true;
        this.running = false;
        this.pid = null;
        this.version = version;
        this.exitCode = null;
        this.manualStop = false;
        this.summary = null;
        this.lines = [];
        this.emit();
    },

    started(pid) {
        this.launching = false;
        this.running = true;
        this.pid = pid ?? null;
        this.exitCode = null;
        this.emit();
    },

    fail() {
        this.launching = false;
        this.running = false;
        this.emit();
    },

    push(line) {
        const text = String(line ?? '').replace(/\r$/, '');
        this.lines.push(text);
        if (this.lines.length > LINE_LIMIT) {
            this.lines.splice(0, this.lines.length - LINE_LIMIT);
        }
        for (const fn of [...lineSubscribers]) {
            try {
                fn(text);
            } catch (error) {
                console.error('[game-store] line subscriber failed', error);
            }
        }
    },

    subscribe(fn, immediate = true) {
        stateSubscribers.add(fn);
        if (immediate) fn(this);
        return () => stateSubscribers.delete(fn);
    },

    subscribeLines(fn) {
        lineSubscribers.add(fn);
        return () => lineSubscribers.delete(fn);
    },

    emit() {
        for (const fn of [...stateSubscribers]) {
            try {
                fn(this);
            } catch (error) {
                console.error('[game-store] subscriber failed', error);
            }
        }
    },
};
