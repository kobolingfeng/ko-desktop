// 动态壁纸 — 前端控制逻辑
import { dialog, fs, app, tray, menu, media } from './api';

const video = document.getElementById('player') as HTMLVideoElement;
const dbg = document.getElementById('dbg')!;
let currentVideoPath = '';

function log(msg: string) {
    dbg.textContent = (dbg.textContent || '') + '\n' + msg;
    console.log('[wallpaper]', msg);
}

video.addEventListener('error', () => log('VIDEO ERROR: ' + (video.error?.message || video.error?.code)));
video.addEventListener('loadstart', () => log('loadstart'));
video.addEventListener('canplay', () => log('canplay'));
video.addEventListener('playing', () => log('playing'));

// ── Config persistence ───────────────────────

async function loadConfig(): Promise<{ videoPath?: string }> {
    try {
        const dir = await app.dataDir();
        const cfgPath = dir + '\\wallpaper.json';
        if (await fs.exists(cfgPath)) {
            return JSON.parse(await fs.readTextFile(cfgPath));
        }
    } catch {}
    return {};
}

async function saveConfig() {
    try {
        const dir = await app.dataDir();
        await fs.writeTextFile(
            dir + '\\wallpaper.json',
            JSON.stringify({ videoPath: currentVideoPath })
        );
    } catch {}
}

// ── Video playback ───────────────────────────

async function playVideo(filePath: string) {
    try {
        log(`playVideo: ${filePath}`);
        const filename = await media.mapFolder(filePath);
        currentVideoPath = filePath;
        const url = `https://media.localhost/${encodeURIComponent(filename)}`;
        log(`video.src = ${url}`);
        video.src = url;
        const p = video.play();
        p.then(() => log('play() OK')).catch(e => log('play() ERR: ' + e));
        await saveConfig();
    } catch (e: any) {
        log('playVideo ERR: ' + e.message);
    }
}

async function pickVideo() {
    const path = await dialog.openFile({
        filters: [
            { name: '视频文件', extensions: ['mp4', 'webm', 'ogg'] },
            { name: '所有文件', extensions: ['*'] },
        ]
    });
    if (path && typeof path === 'string') {
        await playVideo(path);
    }
}

// ── Tray menu ────────────────────────────────

async function showTrayMenu() {
    const isPaused = video.paused;
    const isMuted = video.muted;
    const idx = await menu.popup([
        { label: '选择视频...' },
        { label: isPaused ? '继续播放' : '暂停', disabled: !currentVideoPath },
        { label: isMuted ? '取消静音' : '静音', disabled: !currentVideoPath },
        '-',
        { label: '退出' },
    ]);
    if (idx === 0) await pickVideo();
    else if (idx === 1) { isPaused ? video.play() : video.pause(); }
    else if (idx === 2) { video.muted = !video.muted; }
    else if (idx === 4) { await app.exit(); }
}

// ── Init ─────────────────────────────────────

(async () => {
    // Create tray icon
    await tray.create('动态壁纸');
    tray.onRightClick(showTrayMenu);
    tray.onDoubleClick(pickVideo);

    // Restore last video, or auto-load sample.mp4 from exe directory
    const cfg = await loadConfig();
    if (cfg.videoPath) {
        try {
            if (await fs.exists(cfg.videoPath)) {
                await playVideo(cfg.videoPath);
            }
        } catch {}
    } else {
        // Try loading sample.mp4 next to the exe
        try {
            const dir = await app.exeDir();
            const sample = dir + '\\sample.mp4';
            if (await fs.exists(sample)) {
                await playVideo(sample);
            }
        } catch (e: any) {
            log('auto-load ERR: ' + e.message);
        }
    }
})();
