# AGENTS.md

## Project

This repository is a long-lived fork of the Godot Engine used to develop
rendering technology and engine features. The primary goal is to keep
the fork maintainable and easy to rebase onto upstream Godot.

## Core Principles

-   Prefer extending the engine over modifying existing systems.
-   Add new files instead of growing existing ones whenever practical.
-   Keep changes localized and modular.
-   Minimize architectural drag and unnecessary dependencies.
-   The engine must never depend on game-specific code.

## Architecture

-   New rendering systems should live in their own subsystem under
    `servers/rendering/renderer_rd/`.
-   Existing renderer classes (such as `RenderForwardMobile`) should
    primarily orchestrate rendering passes rather than contain large
    feature implementations.
-   Favor composition over invasive modifications.
-   Avoid introducing global state.

## Implementation

-   Follow `../COMMENTING_GUIDE.md` for file summaries, API briefs,
    default-value rationale, block comments, and the final comment sanity check.
-   Before making significant architectural changes, explain the
    reasoning briefly.
-   When multiple reasonable approaches exist, briefly explain the
    trade-offs and recommend one.
-   Match existing Godot coding conventions unless instructed otherwise.
-   Keep functions focused, readable, and explicit.
-   Avoid unnecessary abstractions.
-   Prefer the least invasive solution that satisfies the requirements.

## Git

-   Never push changes unless explicitly requested.
-   Never rewrite Git history (`reset --hard`, `rebase`, `force-push`)
    without approval.
-   Use `git status` and `git diff` frequently to review work.
-   Work in small, coherent changes.
-   After completing a feature, fixing a bug, or reaching a stable
    build, suggest creating a commit.
-   Only create a commit after user approval.
-   Write clear, concise commit messages.

## Milestones

Suggest creating a commit when:

-   The project builds successfully.
-   A feature is complete and verified.
-   A bug has been fixed.
-   A refactor is complete without changing behavior.
-   Before starting a larger unrelated task.

Do not suggest commits while the project does not build or work is
incomplete.

## Safety

-   Never delete or rename large parts of the codebase without approval.
-   Ask before broad refactors spanning multiple subsystems.
-   When uncertain, ask rather than assume.

## Validation

Before considering work complete:

-   Ensure the project builds successfully.
-   Check for new compiler warnings or errors.
-   Summarize what changed and why.
-   Mention assumptions, limitations, and recommended next steps.

## Local Build Environment

-   The optional AccessKit, Direct3D 12, and ANGLE dependency packages are not
    installed locally. Disable those drivers explicitly when building instead
    of allowing SCons to stop during configuration:

    ```powershell
    scons platform=windows target=editor dev_build=yes accesskit=no d3d12=no angle=no -j6
    ```

-   A running `godot.windows.editor.dev.x86_64.exe` locks the editor executable
    and prevents SCons from completing the final link. Close the running editor
    before a validation build that needs to replace the executable.
