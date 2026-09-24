# 内部構成

## モジュール

| ファイル | 担当 |
|---|---|
| `src/main.c` | CLI、XDGパス、終了コード、初期化・終了処理 |
| `src/config.c` | `feeds.txt`、タグ、URL・重複の検証 |
| `src/util.c` | 文字列、日付、URL、ファイル操作、共通初期化 |
| `src/parser.c` | RSS 1.0 / RSS 2.0 / Atom、識別子、HTML→検索用テキスト |
| `src/fetch.c` | pthreadワーカー、libcurl、結果キュー、条件付き取得 |
| `src/storage.c` | SQLiteスキーマ、索引、記事・月・検索・キャッシュ |
| `src/import.c` | 旧myrss JSONの検証と取込 |
| `src/ui.c` | GTKウィジェット、非同期ジョブ、操作、表示設定 |
| `src/reader.c` | 内蔵HTMLサブセットのGtkTextBufferへの描画 |

`include/feedman.h` はGUIに依存しない公開インターフェースです。`feedman_core` はGTKなしでビルド・テストできます。GUI専用の読取ビューアーは `src/reader.h` に分けています。

## 取得パイプライン

```text
GUIの取得ボタン / CLI --fetch
  → feeds.txt検証 → DB別の排他ロック
  → コーディネーター: 設定同期・HTTPキャッシュ読込
  → 固定数のpthreadワーカー
       URLを動的に担当 → HTTP取得 → RSS/Atom解析
       → 容量制限付きの結果キュー
  → コーディネーターがキューから受信
       → フィード単位のSQLiteトランザクション
  → ワーカー終了を回収 → 統計・エラーを返却
```

URLごとに無制限にスレッドを作りません。ワーカーごとのCURL easy handleを再利用し、CURL shareはDNS / TLSセッションのみに使用します。共有ロックを実装し、接続プールそのものは共有しません。`curl_global_init` と `xmlInitParser` はワーカー起動前に実行します。

DBを各ネットワークワーカーから同時に書き換えるのではなく、取得結果を1つのコーディネーターへ戻すことで、SQLiteの書込競合を抑えています。解析成功と保存成功を確認してからETag等を更新するため、不正XMLのキャッシュで更新を取り逃すのを防ぎます。

## GUIの非同期処理

GTKの呼出しはメインスレッドに限定します。記事一覧・件数・月の印、記事本文、既読保存は `GTask` のワーカーへ渡し、各ジョブが独立したSQLite接続を使用します。選択変更で古いジョブをキャンセルし、最新ではないコールバックの結果を画面に反映しません。

全件の本文をリストモデルへ持たせません。一覧はID・タイトル・時刻・タグ・配信元・既読状態の250件分、本文は選択された1件です。検索は180msのデバウンスとSQLiteのprogress handlerによる中断を組み合わせています。

終了時には、ウィンドウを隠し、検索・取得を停止要求し、未完了ジョブのコールバックが終わるまでGUI状態を解放しません。記事を素早く切り替えてからダブルクリックしても、前の記事URLを開かないようにしています。

## データモデル

`feeds.url` をキーにタグ・タイトル・ETag・Last-Modified・最終確認時刻を保存します。`articles` は `(feed_url, entry_key)` を一意キーとし、タイトル、リンク、HTML、検索用テキスト、公開日時、初回取得日時、両日付の分類、既読、旧データ移行フラグを保持します。

フィードのタグ変更は `feeds` と一覧のJOINに反映されるため、各記事をタグ変更だけで一括更新する必要はありません。同じ記事の再取得ではHTML等に変化があるものだけ更新し、初回取得日時と既読状態を保持します。

全文索引は外部コンテンツ方式のFTS5 trigramです。検索テキストの変化がない更新では、FTSの更新トリガーも発火させません。FTS5 trigramを作れない環境は文字列走査へフォールバックします。既にFTS付きで作成したDBを、FTS5を完全に欠くSQLiteへ持ち込む移行までは保証しません。

## 設計上の制限

並列化の効果はネットワーク待ち時間、配信元の制限、CPU、ディスクに依存します。メモリー使用量も並列数と記事サイズに比例して増えます。重いフィードでは既定の8より少ないワーカーが適切な場合があります。

短い検索語とFTSなしの検索は走査です。GUI本文の最終描画はGTKメインスレッドで行うため、上限に近い巨大な記事の表示まで遅延ゼロを保証するものではありません。HTMLの完全互換、画像、動画、オフラインでの元サイト再現より、通信抑制と軽量さを優先しています。

## 参照した公開仕様・ドキュメント

- libcurl thread safety: https://curl.se/libcurl/c/threadsafe.html
- libcurl conditional request options: https://curl.se/libcurl/c/CURLOPT_HTTPHEADER.html
- SQLite WAL: https://sqlite.org/wal.html
- SQLite FTS5 / trigram: https://sqlite.org/fts5.html#the_trigram_tokenizer
- GTK 3 Paned: https://docs.gtk.org/gtk3/class.Paned.html
- GTK 3 Calendar: https://docs.gtk.org/gtk3/class.Calendar.html
- libxml2 parser API: https://gnome.pages.gitlab.gnome.org/libxml2/html/parser_8h.html

依存ライブラリーのライセンスはそれぞれのプロジェクトに従います。本リポジトリーのCコードとテストはMITライセンスです。
