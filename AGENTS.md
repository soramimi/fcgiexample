# AGENTS.md

このファイルは、本リポジトリを扱う AI コーディングエージェント向けの情報です。

## プロジェクト概要

`fcgiexample` は、C++ で書かれた最小限の **FastCGI サーバー／アプリケーションのサンプル** です。

- `serv`（`fcgiserv`）: 簡易 HTTP サーバー。リクエストを受けて FastCGI アプリを起動・橋渡しする。
- `app`（`fcgiapp`）: FastCGI 対応アプリケーション。サーバーからのリクエストに応答する。
- `fcgi/`: FastCGI リファレンス実装（libfcgi）を内包。

## ディレクトリ構成

```
.
├── AGENTS.md           本ファイル
├── fcgiapp.pro         app 側の Qt/qmake プロジェクトファイル
├── fcgiserv.pro        serv 側の Qt/qmake プロジェクトファイル
├── fcgi/               FastCGI ライブラリ（libfcgi + include）
├── serv/               HTTP サーバー＋FastCGI ブリッジのソース
├── app/                FastCGI アプリケーションのソース
├── _bin/               ビルド済み実行ファイルの出力先
├── build/              qmake 中間生成物
└── misc/               その他雑ファイル
```

## ビルド

Qt（qmake）を使ってビルドします。コンソールアプリケーションで、Qt ライブラリには依存しません。

```bash
# serv のビルド
qmake fcgiserv.pro
make

# app のビルド
qmake fcgiapp.pro
make
```

または Qt Creator で `fcgiapp.pro` / `fcgiserv.pro` を開いてビルド可能です。

### 注意

- `DESTDIR` は両方とも `$$PWD/_bin` に設定されている。
- Linux では `fcgiserv` のリンクに `-lpthread` が必要。
- Windows 用コードも条件コンパイルで含まれているが、現在の主なターゲットは Linux/Unix。

## 実行

```bash
# 1. サーバーを起動（ポート 5000）
./_bin/fcgiserv

# 2. 別ターミナルでアプリを起動（CGI stdin モード、サーバーが起動する）
./_bin/fcgiapp

# または TCP 待受モードで起動
./_bin/fcgiapp -p 3000

# ブラウザや curl でアクセス
curl http://localhost:5000/app/
curl http://localhost:5000/hello/
```

## 主要コンポーネント

### serv 側

| ファイル | 役割 |
|---|---|
| `main.cpp` | HTTP ハンドラ（`MyHandler`）とサーバー起動。`/app/` で FastCGI を呼び出す。 |
| `httpserver.cpp/h` | 簡易 HTTP/1.1 サーバー。リクエストパース、レスポンス送信、WebSocket ハンドシェイク。 |
| `FcgiProcess.cpp/h` | FastCGI プロセス／ソケット接続の管理。Unix ドメインソケット、INET ソケット、子プロセス起動に対応。 |
| `socket.h` | クロスプラットフォームなソケット型とヘルパー。 |
| `thread.cpp/h`, `event.cpp/h`, `mutex.h` | スレッド・イベント・ミューテックスのユーティリティ。 |
| `misc.cpp/h`, `strformat.h`, `joinpath.cpp/h` | 文字列処理、パス結合など。 |
| `base64.cpp/h`, `sha1.c/h` | WebSocket ハンドシェイク用の Base64/SHA1。 |
| `httpstatus.cpp/h`, `httpstatus.txt` | HTTP ステータスコード定義。 |
| `debug.cpp/h` | デバッグログ出力。 |

### app 側

| ファイル | 役割 |
|---|---|
| `main.cpp` | `FCGI_Accept()` ループ。時刻と環境変数を返す。 |
| `myfcgi.c/h` | TCP または Unix ドメインソケットで待ち受けるためのヘルパー。 |
| `fcgiapp.c` | FastCGI 標準入出力のラッパー実装。 |

## コードスタイル・注意事項

- C++11 を使用（`CONFIG += c++11`）。
- スペースインデント（4 スペース）が基本。
- プラットフォーム分岐は `#ifdef _WIN32` / `#else` で行う。
- `serv/main.cpp` 内の `invoke_fastcgi()` は、FastCGI プロトコルのエンコード／デコードを直接実装している。プロトコル変更時はここを確認すること。
- FastCGI バックエンドの起動方式は `cmd` 文字列で切替可能:
    - `./fcgiapp`（プロセス直接起動）: リクエストごとに `fork()` + `execvp()` で起動。リスニングソケットは1回の `accept()` で消費されるため、毎リクエスト再起動が必要（`proc_expired` フラグで管理）。
    - `unix:/path/to.sock`: Unix ドメインソケット接続。接続済みソケットはリクエスト間で再利用される。
    - `inet:host:port`: TCP 接続。同上。
- デフォルトのバインドアドレスは `127.0.0.1`（ループバックのみ）。外部公開する場合は `HTTP_Server::setBindAddress("0.0.0.0")` を呼び出すこと。

### 既知の堅牢化対応

以下は `serv` 側で既に実施済みの安定性・セキュリティ強化です。新規に変更する際はこれらを損なわないようにしてください。

- **HTTP リクエスト制限**: 1 行あたり 8KiB、ヘッダー全体 64KiB、ボディ 8MiB の上限を設けている。超過時は接続を切断または `413` を返す。
- **ソケットタイムアウト**: `accept()` 後の接続ソケットと FastCGI 上流ソケットに `SO_RCVTIMEO`/`SO_SNDTIMEO`（30秒）を設定し、Slowloris 攻撃や無応答クライアント/上流を防ぐ。`accept()` は `EINTR` を再試行する。
- **URL 正規化・パストラバーサル防止**: `misc::normalize_path()` で `.` / `..` を解決し、不正なパスは `400` を返す。
- **Content-Length 検証**: 重複する `Content-Length` ヘッダーを拒否して `400` を返す（HTTP request smuggling 対策）。不正な値は `413` を返す。
- **HTTP バージョン厳格化**: keep-alive は `HTTP/1.1` 以上（`maj == 1 && min >= 1`）のみ有効。
- **POST ボディの読み捨て**: keep-alive 接続でハンドラがボディを消費しなかった場合、`Content-Length` 分を自動的に読み捨てる。
- **レスポンス送信の信頼性**: `send_all()` で全バイト送信を保証し、失敗時は接続を閉じる。
- **FastCGI プロトコル**: 128 バイト以上のパラメータ長に対応した 4 バイトエンコーディングを実装。params が 65535 バイトを超える場合は複数レコードに分割。応答パースは動的バッファで安全に行う。
- **FastCGI 応答の安全性**: `FCGI_END_REQUEST` レコードを必須とし、応答全体の上限（64MiB）とレコード数上限（100,000）を設けて無限ループ・メモリ枯渇を防止。上流エラー時は `502` を返す。
- **環境変数のサニタイズ**: FastCGI 環境変数の name/value から `\r` `\n` `\0` を含むものを拒否し、ヘッダーインジェクションと C 文字列切り詰めを防止。
- **FCGI 構造体のパック**: `#pragma pack(push, 1)` と `static_assert` で `FCGI_Header` 等のサイズを保証。
- **chdir の RAII**: `invoke_fastcgi()` 内の `chdir()` は `CwdGuard` でスコープ退出時に必ず復元される（早期 return / 例外安全）。
- **子プロセス管理**: `execvp()` 失敗時に `_exit(127)`、`SIGCHLD` でゾンビ防止、デストラクタで `SIGTERM` 送信。`launch()` 失敗時は早期リターン。
- **WebSocket**: RFC 6455 準拠のフレーム解析を実装。拡張ペイロード長、断片化、ping/pong/close に対応。最大メッセージサイズは 1MiB。プロトコルエラー時は Close フレーム（status 1000）を送信。ログ出力時はペイロードの制御文字をサニタイズ。

## Git の状態に関する注意

`serv` 側の複数ファイルと `fcgiapp.pro` が変更済み（未コミット）の状態が確認されている。新たな編集前に `git status` / `git diff` を確認すること。
