import { call } from '../rpc.js';
import { store } from '../store.js';
import { setStatus } from '../ui.js';
import { el } from '../utils.js';

function contextLine(data) {
    const context = data?.context || {};
    const parts = [];
    if (context.game_version) parts.push(`Minecraft ${context.game_version}`);
    if (context.loader) parts.push(context.loader);
    if (context.java_version) parts.push(`Java ${context.java_version}`);
    if (context.max_memory_mb) parts.push(`最大内存 ${context.max_memory_mb} MB`);
    return parts.join(' · ') || '—';
}

function findingLine(finding, index) {
    const lines = [];
    lines.push(`${index + 1}. [${finding.category || 'unknown'}] ${finding.title || ''}` +
        (finding.time ? `（${finding.time}）` : ''));
    if (finding.mod_a || finding.mod_b) {
        lines.push(`   涉及模组：${finding.mod_a || '?'}` +
            (finding.mod_b ? ` ↔ ${finding.mod_b}` : ''));
    }
    if (finding.detail) lines.push(`   ${finding.detail}`);
    if (finding.event) lines.push(`   日志：${finding.event}`);
    if (finding.evidence) {
        lines.push('   上下文：');
        for (const line of String(finding.evidence).split('\n')) {
            lines.push('     ' + line);
        }
    }
    for (const suggestion of finding.suggestions || []) {
        lines.push(`   建议：${suggestion}`);
    }
    return lines;
}

export function buildCrashReport(data) {
    const version = document.getElementById('version')?.textContent || 'CMCL';
    const findings = Array.isArray(data?.findings) ? data.findings : [];
    const lines = [
        'CMCL 崩溃分析报告',
        `生成时间：${new Date().toLocaleString('zh-CN')}`,
        `启动器版本：${version}`,
        `实例：${data?.instance || '—'}`,
        `运行环境：${contextLine(data)}`,
        `退出码：${data?.exit_code ?? '—'}`,
        `运行时长：${data?.duration_seconds ?? '—'} 秒`,
        `分析结论：${data?.title || '—'}`,
        `原因：${data?.detail || '—'}`,
        '',
    ];

    if (findings.length) {
        lines.push(`发现的问题（${findings.length}）：`);
        findings.forEach((finding, index) => {
            lines.push(...findingLine(finding, index));
            lines.push('');
        });
    } else {
        lines.push('未匹配到已知崩溃特征。', '');
    }

    if (data?.crash_report_path) {
        lines.push(`崩溃报告：${data.crash_report_path}`, '');
    }
    lines.push('----- 日志末尾 -----', data?.log_tail || '（无）');
    if (data?.crash_report_tail) {
        lines.push('', '----- 崩溃报告末尾 -----', data.crash_report_tail);
    }
    return lines.join('\n');
}

async function copyText(text) {
    try {
        await navigator.clipboard.writeText(text);
        return true;
    } catch (_) {
        // 回退到旧版复制方式。
    }
    const area = el('textarea', { value: text });
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.append(area);
    area.select();
    try {
        return document.execCommand('copy');
    } catch (_) {
        return false;
    } finally {
        area.remove();
    }
}

export function openCrashReport(data) {
    const text = buildCrashReport(data);
    const overlay = el('div', { className: 'modal-overlay' });
    const modal = el('div', {
        className: 'modal crash-report-modal',
        role: 'dialog',
        'aria-modal': 'true',
    });

    modal.append(
        el('h3', { text: '导出崩溃报告' }),
        el('p', {
            className: 'muted',
            text: '报告可能包含玩家名、文件路径和模组列表。发送前请确认内容；这里不会自动上传。',
        }),
    );

    const area = el('textarea', {
        className: 'crash-report-text',
        readonly: '',
        spellcheck: 'false',
    });
    area.value = text;
    modal.append(area);

    const actions = el('div', { className: 'crash-report-actions' });
    const copy = el('button', {
        className: 'btn primary',
        type: 'button',
        text: '复制报告',
    });
    const save = el('button', {
        className: 'btn ghost',
        type: 'button',
        text: '保存到 crash-reports',
    });
    const openFolder = el('button', {
        className: 'btn ghost',
        type: 'button',
        text: '打开报告文件夹',
    });
    const close = el('button', {
        className: 'btn ghost',
        type: 'button',
        text: '关闭',
    });

    const closeModal = () => overlay.remove();

    copy.addEventListener('click', async () => {
        const ok = await copyText(text);
        setStatus(ok ? '崩溃报告已复制，可直接粘贴发送' : '复制失败，请手动选择文本');
        if (ok) closeModal();
    });

    save.addEventListener('click', async () => {
        save.disabled = true;
        save.textContent = '保存中…';
        try {
            const result = await call('crash.save_report', {
                mc_dir: store.mcDir,
                text,
            });
            setStatus('崩溃报告已保存：' + (result?.path || 'crash-reports'));
            save.textContent = '已保存';
        } catch (error) {
            save.disabled = false;
            save.textContent = '保存到 crash-reports';
            setStatus('保存失败：' + (error?.message || String(error)));
        }
    });

    openFolder.addEventListener('click', async () => {
        try {
            await call('crash.open_folder', { mc_dir: store.mcDir });
            setStatus('已打开 crash-reports 文件夹');
        } catch (error) {
            setStatus('打开文件夹失败：' + (error?.message || String(error)));
        }
    });

    close.addEventListener('click', closeModal);
    overlay.addEventListener('click', (event) => {
        if (event.target === overlay) closeModal();
    });
    document.addEventListener('keydown', function onKey(event) {
        if (event.key === 'Escape') {
            document.removeEventListener('keydown', onKey);
            closeModal();
        }
    });

    actions.append(copy, save, openFolder, close);
    modal.append(actions);
    overlay.append(modal);
    document.body.append(overlay);
    area.focus();
    area.setSelectionRange(0, 0);
}
