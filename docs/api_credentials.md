## API credentials

fuckgram needs its own Telegram API credentials. Do not use the official
Telegram Desktop keys in a third-party client.

1. Sign in at [my.telegram.org](https://my.telegram.org) with your phone number.
2. Open **API development tools** and create an application.
3. Copy **api_id** and **api_hash**.

Pass them when configuring a local build:

    -D TDESKTOP_API_ID=YOUR_API_ID -D TDESKTOP_API_HASH=YOUR_API_HASH

For GitHub Actions, add them as repository secrets named `TDESKTOP_API_ID`
and `TDESKTOP_API_HASH` (Settings → Secrets and variables → Actions). Without
them the workflow builds with test credentials, and such a build cannot sign
in to a regular account.

Never commit the credentials to the repository.
