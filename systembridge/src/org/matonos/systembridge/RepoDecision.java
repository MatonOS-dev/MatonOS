package org.matonos.systembridge;

/** One-shot consent, with deadline and caller death both failing closed. */
final class RepoDecision {
    enum State { PENDING, CONFIRMED, DENIED, TIMED_OUT, CALLER_DIED }
    private State state=State.PENDING;
    synchronized boolean finish(State next) {
        if(state!=State.PENDING || next==State.PENDING)return false;
        state=next;return true;
    }
    synchronized State state(){return state;}
}
