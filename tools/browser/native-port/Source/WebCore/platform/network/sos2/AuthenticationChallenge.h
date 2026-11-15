#pragma once
#include <WebCore/AuthenticationChallengeBase.h>

namespace WebCore {
class AuthenticationClient;

class AuthenticationChallenge final : public AuthenticationChallengeBase {
public:
    AuthenticationChallenge() = default;

    AuthenticationChallenge(const ProtectionSpace &space, const Credential &credential,
                            unsigned failures, const ResourceResponse &response,
                            const ResourceError &error)
        : AuthenticationChallengeBase(space, credential, failures, response, error) { }

    AuthenticationClient *authenticationClient() const { return nullptr; }

    void setProposedCredential(const Credential &credential) { m_proposedCredential = credential; }

private:
    friend class AuthenticationChallengeBase;

    static bool platformCompare(const AuthenticationChallenge &, const AuthenticationChallenge &) {
        return true;
    }
};
}
