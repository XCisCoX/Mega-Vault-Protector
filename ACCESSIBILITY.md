# Accessibility

Mega Vault Protect helps people keep and use a private library of files on their computer or phone. People with different vision, hearing, movement, and cognitive needs should be able to use it and contribute to its development.

This document describes our accessibility priorities, expectations for contributions, and how to report a barrier. These are development goals, not a claim that every feature is accessible. A formal accessibility evaluation has not been documented.

## Priorities

We prioritize access to essential tasks: creating and unlocking a vault, importing and finding files, opening supported content, managing records, and controlling sharing and transfers.

Our goals are to:

- Make desktop tasks usable with a keyboard, with visible focus and a predictable navigation order.
- Give controls meaningful names and expose their purpose, state, and errors to screen readers.
- Keep text readable when users increase text size or display scaling, and avoid using color alone to communicate information.
- Provide alternatives to actions that depend on dragging, pinching, double-tapping, holding a control, or scanning a QR code.
- Keep playback controls discoverable and usable without precise pointer movements.
- Explain errors, confirmations, and progress in plain language, especially when an action deletes files or changes vault access.
- Make accessibility improvements without exposing passwords, secret record values, or private file contents.

## Contributor expectations

User-facing changes should preserve or improve access to the affected task. Prefer controls that work with the platform's accessibility features. Label icon-only buttons, preserve visible focus, and avoid interactions that rely only on color, hover, sound, or gestures.

For affected interfaces, contributors should:

- Try the task using only a keyboard on desktop, including dialogs and returning focus after they close.
- Check larger text and display scaling for clipped text or unreachable controls.
- Check names, navigation, and state announcements with a screen reader where available.
- Check that errors and destructive-action confirmations are understandable and reachable.
- Use sample data when testing secret fields, clipboard behavior, vault locking, and sharing.

Pull requests should describe what was checked, the platform and assistive technology used, the result, and any remaining gaps. If a check could not be performed, say so. Include reproduction steps for any barrier found and document changes to user workflows.

The project's documented automated tests cover core vault and media behavior. Passing those tests does not establish accessibility, and no dedicated accessibility CI check is documented. Run the relevant existing checks alongside manual checks for the changed interface.

## Reporting accessibility issues

[Open an accessibility issue](https://github.com/XCisCoX/Mega-Vault-Protector/issues/new). Include “Accessibility” in the title so maintainers can identify the report. You can also comment on an existing issue about the same barrier.

Useful details include:

- The task you were trying to complete and the screen or control involved.
- What happened and what you expected to happen.
- The app version, operating system, and device.
- Your input method and assistive technology, including versions if known.
- Steps to reproduce the problem and any workaround you found.
- For documentation barriers, the page URL and browser.

Screenshots and recordings are optional. You do not need to disclose a disability or provide every detail to report a problem or ask for help. GitHub reports are public: do not include vault passwords, secret values, private documents, or QR codes containing sharing-session information. Use sample files and redact sensitive details.

### Severity

Maintainers assess priority based on the effect on the task. Reporters do not need to choose a severity or label. The following descriptions guide that assessment; they do not imply fixed resolution deadlines:

- **Blocking:** an essential task cannot be completed, such as being unable to reach the unlock control or dismiss a dialog using the available input method.
- **Major:** a task is possible only with substantial difficulty or an unreliable workaround, such as having to navigate many unnamed controls to find a file.
- **Minor:** a task remains usable but is harder to understand or complete, such as a confusing announcement or an isolated text layout problem.

Barriers involving deletion, secret disclosure, or loss of access receive particular attention.

### How we respond

Maintainers aim to acknowledge reports, ask for missing information when necessary, and use the issue to communicate findings and progress. When a safe workaround is available, it should be described there.

Response and fix times depend on maintainer availability and the complexity of the problem; there is no guaranteed response or resolution deadline. Any estimate should be recorded in the issue and updated if it changes. When a fix is available, reporters are welcome to try it and confirm whether it resolves the affected task. Confirmation is helpful, but is not required from the reporter.

## Ownership and maintenance

The project maintainer, [XCisCoX](https://github.com/XCisCoX), is the initial contact for accessibility reports and coordinates triage, review, and documentation. Contributors share responsibility for accessibility in the interfaces they change.

Review this statement when preparing releases that change user-facing workflows or platform requirements, and when new testing results or barriers become available. If responsibility changes, update this section and transfer open accessibility issues to the new responsible maintainer.

## Supported environments

The README documents the following application platforms. These are build and distribution environments, not verified assistive-technology compatibility claims.

| Platform | Documented environment | Accessibility evaluation status |
| --- | --- | --- |
| Windows | x64 desktop application using Qt 5.12.12 | No published keyboard-only or screen reader evaluation |
| Linux | x64; Ubuntu 24.04 build environment; distributed package requires glibc 2.39 or newer; Qt 5.15 | No published keyboard-only or screen reader evaluation |
| Android | arm64 devices running Android 8 or newer; Kotlin and Jetpack Compose interface | No published TalkBack, switch access, or external-keyboard evaluation |

Screen reader support, magnification, text scaling, alternative input, and specific device combinations need testing. Platform availability alone does not establish that these combinations work. Contributors should record the app version, environment, task, and result when reporting tests.

## Known limitations

An accessibility test matrix has not been published. The documented headless tests do not demonstrate that people can navigate and operate the graphical interfaces with assistive technology.

The README describes interactions that need accessibility review:

- Picture and PDF navigation includes dragging, wheel or pinch zoom, and double-tapping. Equivalent access through a keyboard or assistive technology has not been documented.
- Android video controls include double-tap seeking, press-and-hold speed changes, and fading controls. Access to these actions with TalkBack or alternative input has not been documented.
- Opening a PC vault on a phone uses a QR code. An equivalent flow for someone who cannot scan that code has not been documented.
- Automatic vault locking after five minutes of inactivity and clearing copied secrets after 30 seconds may affect users who need more time. Accessible timing controls or extensions have not been documented.
- Media and document access depends on both the viewer and the imported content. Caption support, readable PDF text for screen readers, and alternative descriptions for images have not been evaluated in a published accessibility report.

These are areas of uncertainty, not confirmed failures on every device. No verified accessibility workarounds or specific tracked issues are linked here yet. Please use the reporting process above to document an encountered barrier; confirmed limitations, safe workarounds, and issue links should be added as they become available.

## Feedback and improvements

Suggestions for this statement or the project's accessibility practices are welcome through [GitHub issues](https://github.com/XCisCoX/Mega-Vault-Protector/issues) and pull requests. For a barrier that affects your current use of the app, follow the reporting process above so maintainers can track the affected task.

Practical testing, clearer documentation, translations, and improvements to accessible controls are all useful contributions.
