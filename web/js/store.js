import { call } from './rpc.js';

const KEY = 'craftlauncher.settings.v2';
const LEGACY_KEY = 'craftlauncher.settings.v1';

const defaults = {
    username: 'Player',
    maxMem: 4096,
    minMem: 512,
    width: 854,
    height: 480,
    fullscreen: false,
    server: '',
    downloadSource: 'auto',
};

export const store = {
    mcDir: '',
    settings: { ...defaults },
    instances: [],
    selected: null,
    appInfo: null,

    load() {
        let saved = {};
        try {
            saved = JSON.parse(localStorage.getItem(KEY) || '{}');
            if (!saved || !Object.keys(saved).length) {
                saved = JSON.parse(localStorage.getItem(LEGACY_KEY) || '{}');
            }
        } catch (_) {
            saved = {};
        }

        if (typeof saved.mcDir === 'string' && saved.mcDir) {
            this.mcDir = saved.mcDir;
        }
        if (typeof saved.selected === 'string' && saved.selected) {
            this.selected = saved.selected;
        }

        const source = saved.settings && typeof saved.settings === 'object'
            ? saved.settings
            : saved;

        const pick = (...keys) => {
            for (const key of keys) {
                if (source[key] !== undefined && source[key] !== null && source[key] !== '') {
                    return source[key];
                }
            }
            return undefined;
        };

        this.settings.username = String(pick('username') ?? defaults.username);
        this.settings.maxMem = Number(pick('maxMem', 'max_mem', 'max-mem')) || defaults.maxMem;
        this.settings.minMem = Number(pick('minMem', 'min_mem', 'min-mem')) || defaults.minMem;
        this.settings.width = Number(pick('width')) || defaults.width;
        this.settings.height = Number(pick('height')) || defaults.height;
        this.settings.server = String(pick('server') ?? defaults.server);
        this.settings.fullscreen = Boolean(pick('fullscreen'));
        const downloadSource = String(pick('downloadSource', 'download_source') ??
                                      defaults.downloadSource);
        this.settings.downloadSource = ['auto', 'official', 'mirror'].includes(downloadSource)
            ? downloadSource
            : defaults.downloadSource;
    },

    save() {
        try {
            localStorage.setItem(KEY, JSON.stringify({
                mcDir: this.mcDir,
                selected: this.selected,
                settings: this.settings,
            }));
        } catch (_) {
            // localStorage 不可用时不影响启动。
        }
    },

    async init() {
        this.load();
        this.appInfo = await call('app.info');
        const version = document.getElementById('version');
        if (version) version.textContent = this.appInfo.version || '0.2.0';
        if (!this.mcDir) this.mcDir = this.appInfo.default_mc_dir || '';
        document.getElementById('mc-path').textContent = this.mcDir;
        this.save();
        return this.appInfo;
    },

    async refreshInstances() {
        if (!this.mcDir) return [];
        const result = await call('instance.list', { mc_dir: this.mcDir });
        this.instances = Array.isArray(result.instances) ? result.instances : [];
        if (result.mc_dir) this.mcDir = result.mc_dir;
        document.getElementById('mc-path').textContent = this.mcDir;
        this.save();
        return this.instances;
    },

    instance(id) {
        return this.instances.find((item) => item.id === id) || null;
    },

    setMcDir(dir) {
        this.mcDir = String(dir || '').trim();
        document.getElementById('mc-path').textContent = this.mcDir;
        this.save();
    },

    launchParams(extra = {}) {
        return {
            mc_dir: this.mcDir,
            username: this.settings.username || 'Player',
            max_mem: this.settings.maxMem,
            min_mem: this.settings.minMem,
            width: this.settings.width,
            height: this.settings.height,
            fullscreen: this.settings.fullscreen,
            server: this.settings.server,
            ...extra,
        };
    },
};
