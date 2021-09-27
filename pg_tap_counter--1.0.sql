\echo Use "CREATE EXTENSION logerrors" to load this file. \quit

CREATE FUNCTION pgtc_show()
    RETURNS void
AS 'MODULE_PATHNAME', 'pgtc_show'
    LANGUAGE C STRICT;

CREATE FUNCTION pgtc_tap()
    RETURNS void
AS 'MODULE_PATHNAME', 'pgtc_tap'
    LANGUAGE C STRICT;

CREATE FUNCTION pgtc_show_by_time(
    start_ts            timestamptz,
    stop_ts             timestamptz)
    RETURNS void
AS 'MODULE_PATHNAME', 'pgtc_show_by_time'
    LANGUAGE C STRICT;