// api.ts — Minimal API wrappers for dynamic wallpaper app
import { invoke, on } from './ipc';

export interface FileFilter {
    name: string;
    extensions: string[];
}

export interface MenuItem {
    label: string;
    disabled?: boolean;
    checked?: boolean;
}

// ── Dialog ───────────────────────────────────
export const dialog = {
    openFile: (opts?: { filters?: FileFilter[]; multiple?: boolean }) =>
        invoke<string | string[] | null>('dialog.openFile', opts ?? {}),
};

// ── File system (config persistence) ─────────
export const fs = {
    readTextFile:  (path: string) => invoke<string>('fs.readTextFile', { path }),
    writeTextFile: (path: string, content: string) => invoke<boolean>('fs.writeTextFile', { path, content }),
    exists:        (path: string) => invoke<boolean>('fs.exists', { path }),
};

// ── App ──────────────────────────────────────
export const app = {
    exit:    (code = 0) => invoke<boolean>('app.exit', { code }),
    dataDir: () => invoke<string>('app.dataDir'),
    exeDir:  () => invoke<string>('app.exeDir'),
};

// ── Tray ─────────────────────────────────────
export const tray = {
    create:        (tooltip = 'App') => invoke<boolean>('tray.create', { tooltip }),
    remove:        () => invoke<boolean>('tray.remove'),
    onClick:       (handler: () => void) => on('tray.click', handler),
    onDoubleClick: (handler: () => void) => on('tray.doubleClick', handler),
    onRightClick:  (handler: () => void) => on('tray.rightClick', handler),
};

// ── Context Menu ─────────────────────────────
export const menu = {
    popup: (items: (MenuItem | '-')[]) =>
        invoke<number | null>('menu.popup', { items }),
};

// ── Global Hotkeys ───────────────────────────
export const hotkey = {
    register:    (id: number, modifiers: number, key: number) =>
        invoke<boolean>('hotkey.register', { id, modifiers, key }),
    unregister:  (id: number) => invoke<boolean>('hotkey.unregister', { id }),
    onTriggered: (handler: (data: { id: number }) => void) => on('hotkey.triggered', handler),
};

export const MOD = { ALT: 1, CONTROL: 2, SHIFT: 4, WIN: 8 } as const;
export const VK = {
    F1: 0x70, F2: 0x71, F3: 0x72, F4: 0x73, F5: 0x74, F6: 0x75,
    SPACE: 0x20, ESC: 0x1B,
} as const;

// ── Media (wallpaper-specific) ───────────────
export const media = {
    /** Map a video file's parent folder to media.localhost, returns filename */
    mapFolder: (path: string) => invoke<string>('media.mapFolder', { path }),
};

// ── DevTools ─────────────────────────────────
export const devtools = {
    open: () => invoke<boolean>('devtools.open'),
};
