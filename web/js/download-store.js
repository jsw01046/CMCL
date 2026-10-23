import { call, onEvent } from './rpc.js';
import { formatError } from './utils.js';

const subscribers = new Set();
let initialised = false;

const STAGE_WEIGHTS = [
    ['版本 JSON', 3],
    ['客户端', 10],
    ['库文件', 15],
    ['游戏资源索引', 2],
    ['游戏资源', 70],
];

export const downloadStore = {
    busy: false,
    mode: '',
    version: '',
    instanceName: '',
    stage: '',
    message: '',
    file: '',
    completed: 0,
    total: 0,
    bytes: 0,
    totalBytes: 0,
    error: '',
    done: false,
    startedAt: 0,
    speed: 0,
    stageStartedAt: 0,
    stageBaseBytes: 0,

    init() {
        if (initialised) return;
        initialised = true;
        onEvent('download.progress', (data) => {
            const stage = String(data?.stage ?? this.stage);
            const stageChanged = stage !== this.stage;
            this.stage = stage;
            this.message = String(data?.message ?? '');
            this.file = String(data?.file ?? '');
            this.completed = Number(data?.completed) || 0;
            this.total = Number(data?.total) || 0;
            const bytes = Number(data?.bytes) || 0;
            this.totalBytes = Number(data?.total_bytes) || 0;

            const now = Date.now();
            if (stageChanged || !this.stageStartedAt || bytes < this.stageBaseBytes) {
                this.stageStartedAt = now;
                this.stageBaseBytes = bytes;
                this.speed = 0;
            } else {
                const seconds = (now - this.stageStartedAt) / 1000;
                if (seconds >= 0.5) {
                    const delta = bytes - this.stageBaseBytes;
                    if (delta >= 0) {
                        // 用当前阶段的平均速度，避免单个大文件造成瞬时值乱跳。
                        this.speed = delta / seconds;
                    }
                }
            }
            this.bytes = bytes;
            this.emit();
        });
    },

    async start(mode, params) {
        if (this.busy) {
            throw { message: '已有下载任务正在进行' };
        }

        this.busy = true;
        this.mode = mode;
        this.version = mode === 'java'
            ? `Java ${params.feature ?? ''}`.trim()
            : (mode === 'loader'
                ? `${params.game_version || ''} · ${params.loader || ''}`.trim()
                : (mode === 'resource'
                    ? (params.project_title || '资源下载')
                    : (mode === 'modpack'
                        ? (params.instance_name || params.project_title || '整合包安装')
                        : (params.version || params.id || ''))));
        this.instanceName = params.name || params.instance_name || this.version;
        this.stage = '准备';
        this.message = mode === 'repair'
            ? '正在准备修复…'
            : (mode === 'java'
                ? '正在获取 OpenJDK 下载地址…'
                : (mode === 'resource'
                    ? '正在获取资源下载地址…'
                    : (mode === 'modpack'
                        ? '正在读取整合包…'
                        : '正在准备下载…')));
        this.file = '';
        this.completed = 0;
        this.total = 0;
        this.bytes = 0;
        this.totalBytes = 0;
        this.error = '';
        this.done = false;
        this.startedAt = Date.now();
        this.speed = 0;
        this.stageStartedAt = this.startedAt;
        this.stageBaseBytes = 0;
        this.emit();

        try {
            const methods = {
                install: 'download.install',
                repair: 'download.repair',
                java: 'java.download',
                loader: 'loader.install',
                resource: 'modrinth.download',
                modpack: 'modpack.install',
            };
            const result = await call(methods[mode] || methods.install, params);
            this.busy = false;
            this.done = true;
            this.speed = 0;
            this.stage = '完成';
            this.message = mode === 'repair'
                ? '修复完成'
                : (mode === 'java'
                    ? 'Java 安装完成'
                    : (mode === 'resource'
                        ? '资源下载完成'
                        : (mode === 'modpack' ? '整合包安装完成' : '安装完成')));
            this.emit();
            return result;
        } catch (error) {
            this.busy = false;
            this.done = false;
            this.speed = 0;
            // 保留出错时所在的阶段，便于计算整体进度和阶段高亮。
            this.error = formatError(error);
            this.message = this.error.split('\n')[0];
            this.emit();
            throw error;
        }
    },

    async cancel() {
        if (!this.busy) return;
        try {
            await call('download.cancel');
            this.message = '正在取消下载…';
            this.emit();
        } catch (error) {
            this.message = formatError(error).split('\n')[0];
            this.emit();
        }
    },

    reset() {
        if (this.busy) return;
        this.busy = false;
        this.mode = '';
        this.version = '';
        this.instanceName = '';
        this.stage = '';
        this.message = '';
        this.file = '';
        this.completed = 0;
        this.total = 0;
        this.bytes = 0;
        this.totalBytes = 0;
        this.error = '';
        this.done = false;
        this.startedAt = 0;
        this.speed = 0;
        this.stageStartedAt = 0;
        this.stageBaseBytes = 0;
        this.emit();
    },

    percent() {
        if (this.totalBytes > 0) {
            return Math.min(100, (this.bytes / this.totalBytes) * 100);
        }
        if (this.total > 0) {
            return Math.min(100, (this.completed / this.total) * 100);
        }
        return 0;
    },

    overallPercent() {
        if (this.done) return 100;
        if (this.mode === 'java' || this.mode === 'loader' ||
            this.mode === 'resource' || this.mode === 'modpack') {
            // 这些任务只有一个连续阶段，直接按当前进度计算。
            return Math.min(99, this.percent());
        }
        const stagePercent = this.percent();
        let base = 0;
        for (const [key, weight] of STAGE_WEIGHTS) {
            if (this.stage === key) {
                return Math.min(99, base + (weight * stagePercent) / 100);
            }
            base += weight;
        }
        if (this.stage === '完成') return 100;
        return Math.min(99, base);
    },

    eta() {
        if (this.speed <= 0 || this.totalBytes <= this.bytes) return 0;
        return (this.totalBytes - this.bytes) / this.speed;
    },

    elapsed() {
        return this.startedAt ? Math.max(0, (Date.now() - this.startedAt) / 1000) : 0;
    },

    subscribe(fn, immediate = true) {
        subscribers.add(fn);
        if (immediate) fn(this);
        return () => subscribers.delete(fn);
    },

    emit() {
        for (const fn of [...subscribers]) {
            try {
                fn(this);
            } catch (error) {
                console.error('[download-store] subscriber failed', error);
            }
        }
    },
};
