import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const root = process.cwd();
const header = fs.readFileSync(path.join(root, "src/editor/EditorApplication.hpp"), "utf8");
const panels = fs.readFileSync(path.join(root, "src/editor/EditorApplicationPanels.cpp"), "utf8");
const frame = fs.readFileSync(path.join(root, "src/editor/EditorApplicationRecovered.cpp"), "utf8");
const bootstrap = fs.readFileSync(path.join(root, "src/editor/EditorApplicationBootstrap.cpp"), "utf8");
const theme = fs.readFileSync(path.join(root, "src/editor/frontend/ForgeTheme.cpp"), "utf8");
const control = fs.readFileSync(path.join(root, "src/editor/EditorApplicationControlApi.cpp"), "utf8");

assert.match(header, /bool\s+m_projectTransitionActive\s*\{\s*false\s*\}/,
  "editor must keep explicit launcher-to-project transition state");
assert.match(panels, /void\s+EditorApplication::begin_project_transition\s*\(/,
  "launcher actions must enter through the transition helper");
assert.match(panels, /void\s+EditorApplication::draw_project_loading_screen\s*\(/,
  "transition must render a dedicated full-screen loading surface");
assert.match(panels, /begin_project_transition\(m_currentProjectName\)/,
  "open-project actions must use the transition instead of exposing the editor immediately");
assert.match(frame,
  /projectWarmupFrame\s*=\s*[\s\S]{0,120}?m_projectTransitionStage\s*>=\s*1u/,
  "expensive viewport warm-up must happen only after the loading surface was presented");
assert.match(frame,
  /if\s*\(m_projectTransitionActive\)\s*\{[\s\S]{0,900}?draw_dockspace\(\)[\s\S]{0,900}?draw_project_loading_screen\(\)/,
  "dock/panels must be primed behind the loading surface before reveal");
assert.match(frame,
  /if\s*\(m_projectTransitionActive\s*&&\s*presentResult\s*==\s*VK_SUCCESS\)/,
  "transition stages must advance only after a frame actually reaches presentation");
assert.match(frame,
  /m_projectTransitionActive\s*=\s*false;[\s\S]{0,180}?m_inLauncherMode\s*=\s*false;/,
  "launcher mode must end only after the warm-up/loading sequence completes");

// Launcher/theme regression: don't fall back to the old separator-heavy,
// rectangular desktop-form presentation.
assert.match(panels, /InvisibleButton\("##ProjectCard"/,
  "launcher projects must render as modern project cards");
assert.match(panels, /ImGuiStyleVar_ChildRounding,\s*18\.0f/,
  "launcher surface must keep the rounded card treatment");
assert.doesNotMatch(panels, /Selectable\("##ProjectSelectable"/,
  "launcher must not regress to the old full-row Selectable form widgets");
assert.match(bootstrap, /C:\/Windows\/Fonts\/segoeui\.ttf/,
  "Windows editor should prefer the current platform UI typeface");
assert.match(theme, /s\.WindowBorderSize\s*=\s*0\.0f/,
  "modern shell must not draw legacy borders around every window");
assert.ok(control.indexOf('cmd.rfind("screenshot-ui", 0)') < control.indexOf('cmd.rfind("screenshot", 0)'),
  "full-UI screenshot command must be dispatched before the screenshot prefix");
assert.match(control, /capture_ui_screenshot\(path\)/,
  "UI validation endpoint must capture the composed editor frame");

console.log("editor_project_transition_tests: PASS");
