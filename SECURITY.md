# Security Policy

## Active Versions

The KIZUNANO COIN network is designed to allow peering between multiple versions of the node software, with older versions being periodically de-peered.

## Security Audit

In December 2018 the Nano node codebase was audited by Red4Sec and found to have no critical vulnerabilities. The following vulnerability was resolved:

**Risk**: High  
**Report Location**: Pages 34-35  
**Resolution**: [Pull Request #1563](https://github.com/nanocurrency/nano-node/pull/1563) in [release V17.1](https://github.com/nanocurrency/nano-node/releases/tag/V17.1)  

All other notices from the report were classified as informative and are continuously improved on over time (e.g. code styling). The full report is available here: https://content.nano.org/Nano_Final_Security_Audit_v3.pdf

## Reporting a Vulnerability

To report security issues in the Nano protocol, please send an email to security@nano.org and CC the following security team members. It is strongly recommended to encrypt the email using GPG and the pubkeys below can be used for this purpose.

| GitHub Username | Email | GPG Pubkey |
|-----------------------|--------|-----------------|
| [kizunanocoin](https://github.com/kizunanocoin) | kizunanocoin { at } gmail.com | [kizunanocoin.asc](https://github.com/kizunanocoin/node/blob/develop/etc/gpg/kizunanocoin.asc) |

For details on how to send a GPG encrypted email, see the tutorial here: https://www.linode.com/docs/security/encryption/gpg-keys-to-send-encrypted-messages/.

For general support and other non-sensitive inquiries, please visit https://chat.kizunanocoin.com.
