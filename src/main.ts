// 动态壁纸 — 前端控制逻辑
import { dialog, fs, app, tray, menu, media } from './api';
import { invoke, on } from './ipc';

type PlaybackMode = 'loop-one' | 'loop-list';

const video = document.getElementById('player') as HTMLVideoElement;
let currentVideoPath = '';
let playbackMode: PlaybackMode = 'loop-one';

// ── State sync with native ──────────────────

function reportState() {
    invoke('wallpaper.setState', {
        speed: video.playbackRate,
        volume: video.volume,
        muted: video.muted,
        paused: video.paused,
        videoPath: currentVideoPath,
        playbackMode,
    }).catch(() => {});
}

video.addEventListener('play', () => { reportState(); });
video.addEventListener('pause', () => { reportState(); });
video.addEventListener('volumechange', () => { reportState(); scheduleSave(); });
video.addEventListener('ratechange', () => { reportState(); scheduleSave(); });

// Playlist advance: when a video ends in loop-list mode, load the next one.
// (In loop-one mode video.loop=true and 'ended' doesn't fire.)
video.addEventListener('ended', async () => {
    if (playbackMode !== 'loop-list') return;
    try {
        const lib: Array<{ path: string }> = await invoke('library.load');
        if (!lib || !lib.length) return;
        const idx = lib.findIndex(v => v.path === currentVideoPath);
        const next = lib[(idx + 1) % lib.length];
        if (next && next.path) await playVideo(next.path);
    } catch {}
});

function applyPlaybackMode() {
    video.loop = playbackMode === 'loop-one';
}

// Listen for commands from the panel (via native)
on('wallpaper.play', () => video.play());
on('wallpaper.pause', () => video.pause());
on('wallpaper.restart', () => { video.currentTime = 0; });
on('wallpaper.setSpeed', (d: any) => { video.playbackRate = d.rate; });
on('wallpaper.setVolume', (d: any) => { video.volume = d.volume; });
on('wallpaper.setMuted', (d: any) => { video.muted = d.muted; });
on('wallpaper.setPlaybackMode', (d: any) => {
    playbackMode = d.mode === 'loop-list' ? 'loop-list' : 'loop-one';
    applyPlaybackMode();
    reportState();
    scheduleSave();
});
on('wallpaper.pickVideo', () => pickVideo());
on('wallpaper.setVideo', async (d: any) => {
    if (d.path && d.filename) {
        currentVideoPath = d.path;
        video.src = `https://media.localhost/${encodeURIComponent(d.filename)}`;
        video.play().catch(() => {});
        reportState();
        await saveConfig();
    }
});

// ── Config persistence ───────────────────────

interface WallpaperConfig {
    videoPath?: string;
    volume?: number;
    muted?: boolean;
    speed?: number;
    playbackMode?: PlaybackMode;
}

async function loadConfig(): Promise<WallpaperConfig> {
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
            JSON.stringify({
                videoPath: currentVideoPath,
                volume: video.volume,
                muted: video.muted,
                speed: video.playbackRate,
                playbackMode,
            } satisfies WallpaperConfig)
        );
    } catch {}
}

// Debounce writes — volume slider fires many events; batch them.
let saveTimer: ReturnType<typeof setTimeout> | undefined;
function scheduleSave() {
    if (saveTimer) clearTimeout(saveTimer);
    saveTimer = setTimeout(() => { saveTimer = undefined; saveConfig(); }, 400);
}

// ── Video playback ───────────────────────────

async function playVideo(filePath: string) {
    try {
        const filename = await media.mapFolder(filePath);
        currentVideoPath = filePath;
        const url = `https://media.localhost/${encodeURIComponent(filename)}`;
        video.src = url;
        video.play().catch(() => {});
        reportState();
        await saveConfig();
    } catch {}
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
        { label: '打开面板' },
        '-',
        { label: '选择视频...' },
        { label: isPaused ? '继续播放' : '暂停', disabled: !currentVideoPath },
        { label: isMuted ? '取消静音' : '静音', disabled: !currentVideoPath },
        '-',
        { label: '退出' },
    ]);
    if (idx === 0) await invoke('panel.show');
    else if (idx === 2) await pickVideo();
    else if (idx === 3) { isPaused ? video.play() : video.pause(); }
    else if (idx === 4) { video.muted = !video.muted; }
    else if (idx === 6) { await app.exit(); }
}

// ── Init ─────────────────────────────────────

(async () => {
    await tray.create('动态壁纸');
    tray.onRightClick(showTrayMenu);
    tray.onClick(() => invoke('panel.toggle'));
    tray.onDoubleClick(() => invoke('panel.toggle'));

    // Restore config
    const cfg = await loadConfig();
    if (cfg.volume !== undefined) video.volume = cfg.volume;
    if (cfg.muted !== undefined) video.muted = cfg.muted;
    if (cfg.speed !== undefined) video.playbackRate = cfg.speed;
    if (cfg.playbackMode === 'loop-list') playbackMode = 'loop-list';
    applyPlaybackMode();

    // Restore last video, or auto-load sample.mp4
    if (cfg.videoPath) {
        try {
            if (await fs.exists(cfg.videoPath)) {
                await playVideo(cfg.videoPath);
            }
        } catch {}
    } else {
        try {
            const dir = await app.exeDir();
            const sample = dir + '\\sample.mp4';
            if (await fs.exists(sample)) {
                await playVideo(sample);
            }
        } catch {}
    }

    reportState();
})();
