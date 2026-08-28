# Replay datasets

## Required source properties

A replay data set must have an explicit source URL, license or terms that permit the intended use, download date, file checksum, symbol, date, and exact preprocessing command.
Keep the original download outside the repository unless its license explicitly permits redistribution.
Do not describe a sample as a full trading day unless the source says it is one.

## Indian-market preference

NSE and BSE public historical pages expose end-of-day market reports such as Bhav Copy.
Those reports are not message-level order-book feeds and cannot be converted into this project's add, replace, delete, and trade stream without inventing order-book events.
They are therefore unsuitable as a fidelity replay source.

When a license-clean NSE, BSE, or MCX message-level sample becomes available, record it here before use.
The record must include its governing terms and a SHA-256 checksum of the downloaded source file.

## Current fallback

LOBSTER publishes free sample data for academic and evaluation use at <https://lobsterdata.com/info/DataSamples.php>.
Before using a specific sample, confirm its current terms on the download page and record the exact URL, license text or applicable terms, download date, symbol, date, and SHA-256 below.
The sample is not Indian-market data and is not represented as such.

| Field | Required recorded value before replay |
| --- | --- |
| Source URL | Exact downloaded file URL |
| Terms | Exact applicable license or terms URL and version/date |
| Download date | UTC date |
| Instrument | Source symbol and venue |
| Market date | Source market date and available session interval |
| SHA-256 | `sha256sum` output for the original file |
| Preprocessing command | Versioned command that creates normalized fixed frames |

## Normalized preprocessing

The normalized input is a sequence of 64-byte market-data frames.
Preprocessing must preserve source sequence order, map prices to integer ticks, and reject rows that cannot map to the supported add, replace, delete, or trade forms without inference.
The preprocessing tool and its version must be recorded with the data-set manifest.

Verify the source checksum before preprocessing:

```sh
shasum -a 256 path/to/original-source-file
```

Then retain the normalized file checksum and replay command with the benchmark or correctness artifact.