start_server {
    tags {"set"}
    overrides {
        "set-max-intset-entries" 512
        "set-max-listpack-entries" 128
        "set-max-listpack-value" 32
    }
} {
    test {SREM on a missing key returns 0} {
        r del missing-set{t}
        assert_equal 0 [r srem missing-set{t} member]
    }

    test {SMOVE with a missing source does not touch a wrongtype destination} {
        r del src-missing{t} dst-string{t}
        r set dst-string{t} value

        assert_equal 0 [r smove src-missing{t} dst-string{t} member]
        assert_equal value [r get dst-string{t}]
    }

    test {SMOVE with a missing member does not touch a wrongtype destination} {
        r del src-set{t} dst-string{t}
        r sadd src-set{t} existing
        r set dst-string{t} value

        assert_equal 0 [r smove src-set{t} dst-string{t} missing]
        assert_equal {existing} [lsort [r smembers src-set{t}]]
        assert_equal value [r get dst-string{t}]
    }

    test {SMOVE returns 1 when the destination already contains the member} {
        r del src-set{t} dst-set{t}
        r sadd src-set{t} member
        r sadd dst-set{t} member

        assert_equal 1 [r smove src-set{t} dst-set{t} member]
        assert_equal {} [r smembers src-set{t}]
        assert_equal {member} [lsort [r smembers dst-set{t}]]
    }
}
