# Ely.by setup

1. Create an OAuth application at `https://account.ely.by/dev/applications/new`.
2. Select the `Website` application type.
3. Set the redirect URI exactly to `http://127.0.0.1:17843/ely/callback`.
4. Configure the launcher process with the application credentials:

```bat
set AMETHYST_ELY_CLIENT_ID=your_client_id
set AMETHYST_ELY_CLIENT_SECRET=your_client_secret
build\AmethystLauncher.exe
```

The client secret is read from the environment and is not stored in the source code.
The launcher opens Ely.by in the browser, verifies OAuth `state`, receives the callback
on localhost, and passes the Ely.by username, UUID, and access token to Minecraft.
