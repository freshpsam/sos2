# Public Suffix List

`public_suffix_list.dat` is the unmodified list from
https://publicsuffix.org/list/public_suffix_list.dat, version
`2026-11-06_09-01-47_UTC`, upstream commit
`94ec5df7273926560d91e77c002000ba1a53fec4`. It includes both ICANN and private
domains and retains its MPL-2.0 license notice.

`rules.inc` is the sorted ASCII representation compiled into the native cookie
policy. `tools/browser/generate_public_suffixes.py` converts Unicode labels with
Python idna 3.11, using nontransitional UTS #46 and STD3 rules. Normal builds do
not need that Python package or network access. Checksums and the rule count
are recorded in `manifest.json`. Update the source and regenerate explicitly
when updating browser security data.

The matcher handles exact rules, wildcard rules, exceptions and unknown TLDs.
SOS2 additionally treats its reserved `sos2.local` namespace as a public suffix,
so one local page cannot set a parent-domain cookie for its neighbors.
