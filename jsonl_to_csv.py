"""Convert Amazon All Beauty JSONL files to CSV for PostgreSQL \copy import."""

import csv
import json
import sys

DATA_DIR = "/Users/peipeiwang/amazon_data/raw"
OUT_DIR = "/Users/peipeiwang/post_db"


def normalize_product(r: dict) -> dict:
    price_raw = r.get("price")
    try:
        price = float(str(price_raw).replace("$", "").replace(",", "").strip())
    except (ValueError, TypeError):
        price = None

    desc = r.get("description", "") or ""
    if isinstance(desc, list):
        desc = " ".join(desc)

    feats = r.get("features", "") or ""
    if isinstance(feats, list):
        feats = " | ".join(feats)

    return {
        "parent_asin": str(r.get("parent_asin", "") or ""),
        "title": str(r.get("title", "") or "")[:300],
        "main_category": str(r.get("main_category", "") or ""),
        "store": str(r.get("store", "") or ""),
        "average_rating": r.get("average_rating"),
        "rating_number": r.get("rating_number"),
        "price": price,
        "description": str(desc)[:500],
        "features": str(feats)[:500],
    }


def convert_products():
    src = f"{DATA_DIR}/meta_categories/meta_All_Beauty.jsonl"
    dst = f"{OUT_DIR}/products.csv"
    fields = ["parent_asin", "title", "main_category", "store",
              "average_rating", "rating_number", "price", "description", "features"]

    count = 0
    with open(src, "r", encoding="utf-8") as fin, \
         open(dst, "w", newline="", encoding="utf-8") as fout:
        writer = csv.DictWriter(fout, fieldnames=fields)
        writer.writeheader()
        for line in fin:
            line = line.strip()
            if not line:
                continue
            try:
                row = normalize_product(json.loads(line))
                writer.writerow(row)
                count += 1
            except json.JSONDecodeError:
                continue
    print(f"Products: {count:,} rows -> {dst}")


def convert_reviews():
    src = f"{DATA_DIR}/review_categories/All_Beauty.jsonl"
    dst = f"{OUT_DIR}/reviews.csv"
    fields = ["asin", "parent_asin", "user_id", "rating",
              "review_title", "review_text", "review_timestamp",
              "helpful_vote", "verified_purchase"]

    count = 0
    with open(src, "r", encoding="utf-8") as fin, \
         open(dst, "w", newline="", encoding="utf-8") as fout:
        writer = csv.DictWriter(fout, fieldnames=fields)
        writer.writeheader()
        for line in fin:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
                row = {
                    "asin": str(r.get("asin", "") or ""),
                    "parent_asin": str(r.get("parent_asin", "") or ""),
                    "user_id": str(r.get("user_id", "") or ""),
                    "rating": r.get("rating"),
                    "review_title": str(r.get("title", "") or "")[:300],
                    "review_text": str(r.get("text", "") or "")[:2000],
                    "review_timestamp": r.get("timestamp"),
                    "helpful_vote": r.get("helpful_vote", 0),
                    "verified_purchase": r.get("verified_purchase", False),
                }
                writer.writerow(row)
                count += 1
            except json.JSONDecodeError:
                continue
    print(f"Reviews: {count:,} rows -> {dst}")


if __name__ == "__main__":
    convert_products()
    convert_reviews()
