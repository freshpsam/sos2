# Mozilla trust anchors

`mozilla.pem` is curl's unmodified conversion of Mozilla's root certificate
store, dated 2026-11-05: https://curl.se/ca/cacert-2026-11-05.pem

SHA-256: `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285`

Source and license information: https://curl.se/docs/caextract.html
The certificate data is licensed under MPL 2.0: https://mozilla.org/MPL/2.0/.
The PEM retains its upstream notices. Firefox-specific policy constraints not
encoded in certificates are not included in this conversion.

The build verifies this digest and packs the anchors into the immutable system
bundle. Web content cannot add roots. Updating the store is an explicit source
update, not a network download during a build or a page load.
