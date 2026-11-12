// Types shared by the page modules (checked by `tsc` through // @ts-check; the
// page itself runs the plain .js files). The host side of each type is named
// beside it; keep the two in step.

import type en from '../locales/en.js';

/** Every message key, from the English locale (the source of truth). */
export type Messages = typeof en;
export type MessageKey = keyof Messages;
/** A locale other than English: any subset of the English keys. */
export type Translation = Partial<Record<MessageKey, string>>;
export type Params = Record<string, string | number>;
export type Translate = (key: MessageKey, params?: Params) => string;

/** Text the host sends as a key and its parameters (Rust: message::Message). */
export interface HostMessage {
  key: string;
  params?: Params;
}
/** A message from the host, or plain text (process output, the build script). */
export type HostText = HostMessage | string;

/** A game executable the host found (Rust: game::Build). */
export interface Build {
  id: string;
  label: string;
  update: boolean;
  extras: boolean;
  /** Built from a standalone launcher's Development sources (its own commit). */
  development?: boolean;
  /** The Shadow renderer can be chosen (it is in the executable and its shader
   * converter is staged beside it). */
  shadow?: boolean;
  /** Takes --pc_button_glyphs (releases before Button Icons do not). */
  glyphs?: boolean;
  executable: string;
  gameDir?: string | null;
}

/** Injected as window.LAUNCHER_CAPS (Rust: host::Caps). */
export interface Caps {
  platform: 'linux' | 'windows' | 'macos';
  /** Renderers the page may offer. */
  backends?: string[];
  builds: Build[];
  defaults: Record<string, string>;
  canBuild: boolean;
  /** A standalone launcher can build Development (false when it was built from
   * modified or unknown sources, which it cannot fetch). */
  canBuildDevelopment?: boolean;
  updateInstalled?: boolean;
  version?: { commit: string; dirty: boolean; label: string };
  /** The release a standalone launcher builds by default (launcher/stable.json). */
  stable?: { tag: string; commit: string };
  /** Outside a source checkout: builds come from `stable`, or from Development,
   * the launcher's own commit (`version`). */
  standalone?: boolean;
}

/** launcher.json, injected as window.LAUNCHER_SETTINGS (Rust: game::Settings). */
export interface Settings {
  gameDir: string;
  gameDirUpdate: string;
  userDataRoot: string;
  userDataRootUpdate: string;
  /** Save folders of Development builds, apart from the normal ones. */
  userDataRootDevelopment?: string;
  userDataRootUpdateDevelopment?: string;
  /** The player name from before profiles; names the oldest save's profile. */
  username?: string;
  /** Player 1's profile (an XUID), or 'ingame': chosen in game. */
  profile?: string;
  vulkanDevice: number;
  videoDriver?: string;
  buildIso?: string;
  buildTitleUpdate?: string;
  buildCmake?: string;
  uiState?: UiState | null;
  [key: string]: unknown;
}

/**
 * Menu choices saved in launcher.json's uiState and injected back as
 * window.SAVED_STATE. The short names are stored on disk, so they stay.
 * Most are indexes into the row's choices; -1 is "In-game", "Native" or "Auto".
 */
export interface UiState {
  /** Index into languages.js LANGS. */
  lang: number;
  /** D3D12 render path: 0 ROV, 1 RTV. */
  path: number;
  /** Index into the frame-rate list of the current renderer (state.js FPS). */
  fps: number;
  /** state.js FPS_VERSION when `fps` was saved; older saves are moved up one (migrateFps). */
  fpsVersion?: number;
  /** D3D12 image scaling: 0 FSR, 1 CAS, 2 off. */
  scale: number;
  /** Vulkan image scaling: 0 off, 1 FSR, 2 CAS. */
  vkscale: number;
  /** Internal resolution: -1 in-game, 0..2 for 1x..3x. */
  res: number;
  /** -1 in-game, 0 fullscreen, 1 windowed. */
  display: number;
  /** Mouse drives the camera: 0 off, 1 on. */
  mouse: number;
  /** Button icons: 0 Xbox, 1 PlayStation, 2 auto. */
  glyphs: number;
  /** Vulkan render path: 0 FSI, 1 Host, 2 Shadow. */
  vkpath: number;
  /** -1 auto, 0 off, 1 on. */
  vsyncMode: number;
  /** Windows renderer: 0 D3D12, 1 Vulkan, 2 Shadow. */
  backend: number;
  /** Selected Build id. */
  build: string | null;
  /** Shadow render resolution: -1 native, 0..3 presets, 9 custom. */
  shres: number;
  /** Shadow aspect ratio: -1 auto, else an index into SHADOW_ASPECTS. */
  shaspect: number;
  /** Custom Shadow render height in lines. */
  shcustom: number;
  /** Shadow dynamic resolution: 0 off, 1 on. */
  shdyn: number;
}

export type SavedState = Partial<UiState> & { username?: string };

/** The live menu state. */
export interface MenuState extends UiState {
  /** Player 1's profile (an XUID), from settings.profile. */
  profile: string;
  view: 'main' | 'remap' | 'profiles';
  binds: Record<string, string>;
}

/** Sent with Play (Rust: game::GameOptions). */
export interface LaunchConfig {
  shadow: boolean;
  shadowNative: boolean;
  shadowDynamic: boolean;
  shadowHeight: number;
  shadowAspect: string;
  build: string | null;
  vkPath: string;
  rtp: string;
  fps: number;
  backend: string;
  vsync: boolean;
  mnkMode: boolean;
  mnkMouse: boolean;
  glyphs: string;
  present: string;
  resScale: number;
  fullscreen: boolean | null;
  lang: number;
  langId: number;
  countryId: number;
}

/** A build tool the host looked for (Rust: tools::Tool). */
export interface Tool {
  id?: string;
  label: string;
  found: boolean;
  path?: string;
  url?: string;
  note?: HostText;
  winget?: string;
}

/** Rust: tools::ToolReport. */
export interface ToolReport {
  tools: Tool[];
  sdk: Component;
  /** The shader converter (XenosRecomp); absent for sources from before it was
   * part of every build. */
  converter?: Component | null;
  sources?: { repository: string; commit: string } | null;
  warnings?: HostText[];
  ready: boolean;
  winget?: boolean;
}

/** A pinned component the build fetches and builds once (Rust: tools::Sdk). */
export interface Component {
  installed: boolean;
  stale?: boolean;
  repository?: string;
  commit?: string;
}

/** Rust: game::BuildRequest. */
export interface BuildRequest {
  iso: string;
  titleUpdate: string;
  cmake: string;
  variants: string[];
  downloadSdk: boolean;
  downloadSources: boolean;
  /** Standalone launchers: "stable" or "development". */
  channel: string;
}

/** Tauri commands (Rust: commands/*) with their arguments and results. */
export interface Commands {
  save_settings: { args: { settings: Settings }; result: void };
  launch_game: { args: { settings: Settings; options: LaunchConfig }; result: void };
  start_build: { args: { settings: Settings; request: BuildRequest }; result: Build[] };
  check_build_tools: { args: { cmake: string; channel: string }; result: ToolReport | null };
  install_tools: { args: { cmake: string }; result: ToolReport | null };
  cancel_build: { args: undefined; result: void };
  pick_file: { args: { kind: string }; result: string | null };
  open_external: { args: { url: string }; result: void };
  window_action: { args: { action: string }; result: void };
  development_saves: { args: { settings: Settings; build: string }; result: { from: string; to: string } | null };
  prepare_saves: { args: { settings: Settings; build: string; copy: boolean }; result: void };
  list_profiles: { args: { settings: Settings; build: string }; result: Profile[] };
  create_profile: { args: { settings: Settings; build: string | null | undefined; name: string }; result: Profile };
  rename_profile: { args: { settings: Settings; build: string | null | undefined; xuid: string; name: string }; result: Profile };
  delete_profile: { args: { settings: Settings; build: string | null | undefined; xuid: string }; result: void };
}

export interface Host {
  invoke<K extends keyof Commands>(command: K, args?: Commands[K]['args']): Promise<Commands[K]['result']>;
  listen(event: string, handler: (event: { payload: string }) => void): unknown;
}

/** Rules derived from the caps and the menu state (state.js createRules). */
export interface Rules {
  linux: boolean;
  /** macOS: Vulkan through MoltenVK, Shadow renderer only. */
  mac: boolean;
  /** Windows with both renderers in one executable: a Renderer row. */
  winVk(): boolean;
  /** The Vulkan rows apply (always on Linux and macOS). */
  vk(): boolean;
  curBuild(): Build | null;
  extras(): boolean;
  /** The selected build offers Button Icons. */
  glyphs(): boolean;
  /** The Shadow renderer may be offered for the selected build. */
  shadowOk(): boolean;
  /** The Shadow renderer is chosen. */
  shadow(): boolean;
  fpsList(): number[];
  customHeight(): number;
}

/** A local profile (Rust: game::Profile). */
export interface Profile {
  /** 16 hex digits: the profile's save folder. */
  xuid: string;
  name: string;
}

/** profiles.js */
export interface ProfilesView {
  follow(buildId: string | null): void;
  render(): void;
  list(): Profile[];
}

/** Shared by the page modules; built once by main.js start(). */
export interface Context {
  win: Window & typeof globalThis;
  doc: Document;
  caps: Caps;
  settings: Settings;
  state: MenuState;
  rules: Rules;
  /** Local profiles, on hosts that keep them (profiles.js). */
  profiles?: ProfilesView;
  host: Host;
  t: Translate;
  /** Text for a host message, error or plain string, in the current language. */
  describe(value: unknown): string;
  showStatus(text: string): void;
  /** Runs `fn` now and whenever the launcher's language changes. */
  onLanguage(fn: () => void): void;
  /** Redraws the menu (main or remap view) and the language button. */
  render(): void;
}

declare global {
  interface Window {
    __TAURI__: any;
    LAUNCHER_CAPS: Caps;
    LAUNCHER_SETTINGS: Settings;
    SAVED_STATE?: SavedState;
  }
}
