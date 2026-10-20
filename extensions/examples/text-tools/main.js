"use strict";

// Portable JavaScript only: no Node, DOM, network, or filesystem APIs.
// Each invocation receives just the text the user explicitly submitted.
function inputText(input) {
    if (!input || typeof input.text !== "string") {
        throw new Error("Enter some text first.");
    }
    if (input.text.length > 65536) {
        throw new Error("This example accepts up to 65,536 UTF-16 code units.");
    }
    return input.text;
}

globalThis.extension = Object.freeze({
    commands: Object.freeze({
        statistics(input) {
            const text = inputText(input);
            const trimmed = text.trim();
            const words = trimmed ? trimmed.split(/\s+/u).length : 0;
            const lines = text ? text.split(/\r\n|\r|\n/u).length : 0;
            const characters = Array.from(text).length;
            return {
                title: "Text statistics",
                text: "Words (whitespace-separated): " + words +
                    "\nCharacters (Unicode code points): " + characters +
                    "\nLines: " + lines,
            };
        },
        "clean-lines"(input) {
            const text = inputText(input);
            let lines = text.split(/\r\n|\r|\n/u).map(line => line.trim());
            if (!input.settings || input.settings["remove-empty-lines"] !== false) {
                lines = lines.filter(Boolean);
            }
            return {
                title: "Cleaned lines",
                text: lines.join("\n"),
            };
        },
        "format-json"(input) {
            const text = inputText(input);
            const indent = input.settings && input.settings["compact-json"] === true ? 0 : 2;
            return {title: "Formatted JSON", text: JSON.stringify(JSON.parse(text), null, indent)};
        },
    }),
});
