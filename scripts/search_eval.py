#!/usr/bin/env python3
from __future__ import annotations
import hashlib
import json
import math
from pathlib import Path

def corpus_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def reciprocal_rank(grades: list[int]) -> float:
    for idx, grade in enumerate(grades, 1):
        if grade > 0:
            return 1.0 / idx
    return 0.0

def dcg(grades: list[int], k: int = 10) -> float:
    score = 0.0
    for idx, grade in enumerate(grades[:k], 1):
        score += ((2 ** int(grade)) - 1) / math.log2(idx + 1)
    return score

def ndcg(grades: list[int], k: int = 10) -> float:
    ideal = sorted((int(x) for x in grades), reverse=True)
    denom = dcg(ideal, k)
    return dcg(grades, k) / denom if denom else 0.0

def precision(grades: list[int], k: int = 10) -> float:
    window = grades[:k]
    if not window:
        return 0.0
    return sum(1 for x in window if int(x) > 0) / float(k)

def evaluate_query(result_unit_codes: list[str], relevance: dict[str, int], k: int = 10) -> dict[str, float]:
    grades = [int(relevance.get(code, 0)) for code in result_unit_codes]
    return {
        "rr": reciprocal_rank(grades),
        "ndcg10": ndcg(grades, k),
        "precision10": precision(grades, k),
    }

def aggregate(rows: list[dict[str, float]]) -> dict[str, float]:
    if not rows:
        return {"mrr": 0.0, "ndcg10": 0.0, "precision10": 0.0}
    n = float(len(rows))
    return {
        "mrr": sum(x["rr"] for x in rows) / n,
        "ndcg10": sum(x["ndcg10"] for x in rows) / n,
        "precision10": sum(x["precision10"] for x in rows) / n,
    }

def load_corpus(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("ranking_version") != "search-v1":
        raise ValueError("unexpected ranking_version")
    if not isinstance(value.get("inventory"), list) or not isinstance(value.get("queries"), list):
        raise ValueError("invalid corpus")
    return value

def ppm(value: float) -> int:
    return max(0, min(1_000_000, int(round(float(value) * 1_000_000))))

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus", type=Path)
    args = ap.parse_args()
    corpus = load_corpus(args.corpus)
    print(json.dumps({
        "ranking_version": corpus["ranking_version"],
        "corpus_sha256": corpus_sha256(args.corpus),
        "queries": len(corpus["queries"]),
        "inventory": len(corpus["inventory"]),
    }, separators=(",", ":"), sort_keys=True))
