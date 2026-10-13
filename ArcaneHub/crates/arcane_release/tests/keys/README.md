# Test manifest keys

`test-manifest-1` and `test-manifest-2` are TEST keys. Their password is
`arcane-test-key-not-a-secret`, and they are committed on purpose. Tests,
golden manifests, dry runs and staging rehearsals use them. No Hub trusts
them: `keys::TRUSTED_MANIFEST_KEYS` must never contain them, and
`verify_test` checks this. Key 2 exists to test key rotation.
