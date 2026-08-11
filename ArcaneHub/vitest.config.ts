import { defineConfig } from "vitest/config";

// Standalone Vitest config, deliberately NOT the SvelteKit vite.config: these
// are pure-function unit tests and do not need (or want) the SvelteKit plugin
// in the pipeline. Same shape as Tools/setup-wizard/vitest.config.ts.
export default defineConfig({
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
});
