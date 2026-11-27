from collections import Counter
import re


def intra_requests_processed(requests, websock_log, topic, expected_count):
    processed = re.findall(r"\bsHandleStagedEncodedIntraMessage [^\n]*channel_name:'UFSRV:INTRA:" + re.escape(topic) +
                           r"', reqid_target:'\d+', reqid_processed:'(\d+)'[^\n]*PROCESSED (?:TARGET|OTHER) REQID\b",
                           websock_log)
    return (expected_count > 0 and len(set(requests)) == len(requests) == expected_count and
            Counter(consumed for consumed in processed if consumed in requests) == Counter(requests))
