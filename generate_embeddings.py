"""Generate embeddings for beauty products and load into PostgreSQL pgvector column."""

import csv
import struct
import subprocess
import sys

import numpy as np
from sentence_transformers import SentenceTransformer


def main():
    model = SentenceTransformer("all-MiniLM-L6-v2")
    dim = model.get_sentence_embedding_dimension()
    print(f"Model dimension: {dim}")

    csv_path = "/Users/peipeiwang/post_db/products.csv"
    out_path = "/Users/peipeiwang/post_db/product_embeddings.csv"

    rows = []
    with open(csv_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    print(f"Generating embeddings for {len(rows)} products...")

    texts = []
    for row in rows:
        text = f"{row['title']} {row['store']} {row['features']}"[:512]
        texts.append(text)

    batch_size = 256
    all_embeddings = []
    for i in range(0, len(texts), batch_size):
        batch = texts[i:i + batch_size]
        embs = model.encode(batch, show_progress_bar=False, normalize_embeddings=True)
        all_embeddings.append(embs)
        if (i // batch_size) % 10 == 0:
            print(f"  Batch {i // batch_size + 1}/{(len(texts) + batch_size - 1) // batch_size}")

    embeddings = np.vstack(all_embeddings)
    print(f"Embeddings shape: {embeddings.shape}")

    with open(out_path, "w", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["parent_asin", "embedding"])
        for i, row in enumerate(rows):
            vec_str = "[" + ",".join(f"{v:.6f}" for v in embeddings[i]) + "]"
            writer.writerow([row["parent_asin"], vec_str])

    print(f"Written to {out_path}")


if __name__ == "__main__":
    main()
