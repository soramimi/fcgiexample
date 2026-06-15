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
- FastCGI プロセスはリクエストごとに `fork()` + `execvp()` で起動している。性能を求める場合はプロセスの維持（永続化）が必要。

### 既知の堅牢化対応

以下は `serv` 側で既に実施済みの安定性・セキュリティ強化です。新規に変更する際はこれらを損なわないようにしてください。

- **HTTP リクエスト制限**: 1 行あたり 8KiB、ヘッダー全体 64KiB、ボディ 8MiB の上限を設けている。超過時は接続を切断または `413` を返す。
- **URL 正規化・パストラバーサル防止**: `misc::normalize_path()` で `.` / `..` を解決し、不正なパスは `400` を返す。
- **POST ボディの読み捨て**: keep-alive 接続でハンドラがボディを消費しなかった場合、`Content-Length` 分を自動的に読み捨てる。
- **レスポンス送信の信頼性**: `send_all()` で全バイト送信を保証し、失敗時は接続を閉じる。
- **FastCGI プロトコル**: 128 バイト以上のパラメータ長に対応した 4 バイトエンコーディングを実装。応答パースは動的バッファで安全に行う。
- **子プロセス管理**: `execvp()` 失敗時に `_exit(127)`、`SIGCHLD` でゾンビ防止、デストラクタで `SIGTERM` 送信。
- **WebSocket**: RFC 6455 準拠のフレーム解析を実装。拡張ペイロード長、断片化、ping/pong/close に対応。最大メッセージサイズは 1MiB。

## Git の状態に関する注意

`serv` 側の複数ファイルと `fcgiapp.pro` が変更済み（未コミット）の状態が確認されている。新たな編集前に `git status` / `git diff` を確認すること。
