// Playground front end. No framework and no build step: the deploy is a folder
// of static files, which is what keeps the hosting free and the page fast.

(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const sqlBox = $("sql");
  const runButton = $("run");
  const resetButton = $("reset");
  const statusEl = $("status");
  const errorEl = $("error");
  const resultEl = $("result");
  const schemaEl = $("schema");

  let api = null;

  const EXAMPLES = [
    {
      label: "Create and query a table",
      sql: `CREATE TABLE emp (
  id     INTEGER PRIMARY KEY,
  name   TEXT NOT NULL,
  salary REAL
);

INSERT INTO emp VALUES
  (1, 'ada',   120.5),
  (2, 'grace', 140.0),
  (3, 'alan',   99.25),
  (4, 'edsger', 88.0);

SELECT name, salary FROM emp WHERE salary > 100 ORDER BY salary DESC;`
    },
    {
      label: "Nulls are not false",
      sql: `CREATE TABLE n (id INTEGER, v INTEGER);
INSERT INTO n VALUES (1, 10), (2, NULL), (3, 30);

-- Row 2 is dropped by both of these: unknown is not true.
SELECT id FROM n WHERE v > 5;
SELECT id FROM n WHERE v = NULL;`
    },
    {
      label: "Rollback undoes everything",
      sql: `CREATE TABLE acct (id INTEGER, balance INTEGER);
INSERT INTO acct VALUES (1, 100);

BEGIN;
UPDATE acct SET balance = 0 WHERE id = 1;
SELECT balance FROM acct;
ROLLBACK;

SELECT balance FROM acct;`
    },
    {
      label: "An error with a position",
      sql: `SELECT * FROM t WHERE`
    },
    {
      label: "An index changes the plan",
      sql: `CREATE TABLE emp (id INTEGER, name TEXT, salary REAL);
INSERT INTO emp VALUES
  (1,'ada',120.5), (2,'grace',140.0), (3,'alan',99.25), (4,'edsger',88.0);

CREATE INDEX idx_salary ON emp(salary);

-- The plan says IndexScan and "1 of 1 candidates" instead of scanning
-- the table. The Filter stays: the index narrows, the predicate decides.
SELECT name FROM emp WHERE salary = 140;`
    },
    {
      label: "What a query costs in pages",
      sql: `CREATE TABLE big (a INTEGER, b TEXT);

BEGIN;
INSERT INTO big VALUES (1,'x'),(2,'x'),(3,'x'),(4,'x'),(5,'x');
COMMIT;

-- Check the Pages tab: a scan walks the tree for every row, because
-- there is no secondary index and no streaming cursor.
SELECT a FROM big WHERE a > 2;`
    }
  ];

  // --- wasm plumbing --------------------------------------------------------

  function bindApi(module) {
    const call = (name, argTypes) =>
      module.cwrap(name, "string", argTypes || []);
    return {
      open: call("strata_open"),
      exec: call("strata_exec", ["string"]),
      parse: call("strata_parse", ["string"]),
      schema: call("strata_schema")
    };
  }

  function parseJson(text, what) {
    try {
      return JSON.parse(text);
    } catch (e) {
      return { ok: false, error: "could not read the engine's reply to " + what };
    }
  }

  // --- rendering ------------------------------------------------------------

  function setStatus(text, kind) {
    statusEl.textContent = text;
    statusEl.className = "status" + (kind ? " " + kind : "");
  }

  function showError(message) {
    errorEl.textContent = message;
    errorEl.hidden = false;
  }

  function clearError() {
    errorEl.hidden = true;
    errorEl.textContent = "";
  }

  function renderTable(result) {
    resultEl.innerHTML = "";
    if (!result.isQuery) {
      const p = document.createElement("p");
      p.className = "rowcount";
      p.textContent =
        result.rowsAffected > 0
          ? "ok — " + result.rowsAffected + (result.rowsAffected === 1 ? " row" : " rows")
          : "ok";
      resultEl.appendChild(p);
      return;
    }

    const table = document.createElement("table");
    const thead = document.createElement("thead");
    const headRow = document.createElement("tr");
    for (const name of result.columns) {
      const th = document.createElement("th");
      th.textContent = name;
      headRow.appendChild(th);
    }
    thead.appendChild(headRow);
    table.appendChild(thead);

    const tbody = document.createElement("tbody");
    for (const row of result.rows) {
      const tr = document.createElement("tr");
      for (const value of row) {
        const td = document.createElement("td");
        if (value === null) {
          td.textContent = "NULL";
          td.className = "null";
        } else {
          td.textContent = value;
          if (value !== "" && !isNaN(Number(value))) {
            td.className = "num";
          }
        }
        tr.appendChild(td);
      }
      tbody.appendChild(tr);
    }
    table.appendChild(tbody);
    resultEl.appendChild(table);

    const count = document.createElement("p");
    count.className = "rowcount";
    count.textContent = result.rows.length + (result.rows.length === 1 ? " row" : " rows");
    resultEl.appendChild(count);
  }

  function renderPanel(id, text, muted) {
    const pre = document.querySelector("#panel-" + id + " pre");
    pre.textContent = text;
    pre.className = muted ? "muted" : "";
  }

  function renderStats(stats) {
    if (!stats) {
      renderPanel("stats", "No page activity.", true);
      return;
    }
    const lines = [
      "pages in the database   " + stats.pageCount,
      "pages held in cache     " + stats.cachedPages,
      "pages this query asked  " + stats.pageFetches,
      "   ... actually loaded  " + stats.pageLoads,
      "pages live in the log   " + stats.walPages,
      "",
      '"asked" counts every fetch; "loaded" counts the ones that had to come',
      "from the log or the file rather than from cache.",
      "",
      "Loaded will usually read 0 after the first touch, and that is a",
      "limitation rather than a triumph: nothing ever evicts, so the cache",
      "grows to the size of the database. See FUTURE.md, under storage."
    ];
    renderPanel("stats", lines.join("\n"), false);
  }

  function renderTokens(sql) {
    const reply = parseJson(api.parse(sql), "the parser");
    if (!reply.ok) {
      renderPanel("tokens", reply.error || "could not tokenise", true);
      return;
    }
    const rows = reply.tokens.map(
      (t) =>
        String(t.line + ":" + t.column).padEnd(8) +
        t.type.padEnd(22) +
        (t.text || "")
    );
    if (!reply.parsed) {
      rows.push("", "parse error → " + reply.parseError);
    } else if (reply.statements && reply.statements.length) {
      rows.push("");
      for (const s of reply.statements) {
        rows.push(s.kind + (s.where ? "   WHERE " + s.where : ""));
      }
    }
    renderPanel("tokens", rows.join("\n") || "(no tokens)", false);
  }

  function renderSchema() {
    const reply = parseJson(api.schema(), "the catalog");
    if (!reply.ok || !reply.tables || reply.tables.length === 0) {
      schemaEl.innerHTML = '<p class="muted">No tables yet.</p>';
      return;
    }
    schemaEl.innerHTML = "";
    for (const table of reply.tables) {
      const details = document.createElement("details");
      details.open = reply.tables.length <= 2;
      const summary = document.createElement("summary");
      summary.textContent = table.name;
      details.appendChild(summary);

      const ul = document.createElement("ul");
      for (const column of table.columns) {
        const li = document.createElement("li");
        const flags =
          (column.primaryKey ? " pk" : "") + (column.notNull ? " not null" : "");
        li.innerHTML =
          '<span class="cname"></span> <span></span><span class="flag"></span>';
        li.children[0].textContent = column.name;
        li.children[1].textContent = column.type.toLowerCase();
        li.children[2].textContent = flags;
        ul.appendChild(li);
      }
      details.appendChild(ul);
      schemaEl.appendChild(details);
    }
  }

  // --- running --------------------------------------------------------------

  /// Splits on semicolons that are not inside a string literal, so a script
  /// runs statement by statement and the last result is the one displayed.
  function splitStatements(sql) {
    const out = [];
    let current = "";
    let quote = null;
    for (let i = 0; i < sql.length; i++) {
      const c = sql[i];
      if (quote) {
        current += c;
        if (c === quote) {
          if (sql[i + 1] === quote) {
            current += sql[++i];
          } else {
            quote = null;
          }
        }
        continue;
      }
      if (c === "'" || c === '"') {
        quote = c;
        current += c;
        continue;
      }
      if (c === "-" && sql[i + 1] === "-") {
        while (i < sql.length && sql[i] !== "\n") i++;
        current += "\n";
        continue;
      }
      if (c === ";") {
        if (current.trim()) out.push(current.trim());
        current = "";
        continue;
      }
      current += c;
    }
    if (current.trim()) out.push(current.trim());
    return out;
  }

  function run() {
    if (!api) return;
    clearError();

    const text = sqlBox.value;
    const statements = splitStatements(text);
    if (statements.length === 0) {
      setStatus("nothing to run", "");
      return;
    }

    const started = performance.now();
    let last = null;

    for (let i = 0; i < statements.length; i++) {
      const reply = parseJson(api.exec(statements[i]), "a statement");
      if (!reply.ok) {
        showError(
          statements.length > 1
            ? "statement " + (i + 1) + " of " + statements.length + ": " + reply.error
            : reply.error
        );
        // Clear the previous result: leaving it under an error reads as though
        // the failed statement produced it.
        resultEl.innerHTML = "";
        renderPanel("plan", "The statement did not run.", true);
        renderPanel("stats", "The statement did not run.", true);
        setStatus("failed", "bad");
        renderSchema();
        renderTokens(statements[i]);
        return;
      }
      last = reply;
    }

    const elapsed = performance.now() - started;
    renderTable(last);
    renderPanel(
      "plan",
      last.plan && last.plan.length ? last.plan : "No plan — this statement is not a query.",
      !(last.plan && last.plan.length)
    );
    renderStats(last.stats);
    renderTokens(statements[statements.length - 1]);
    renderSchema();
    // Sub-millisecond is the normal case for a small table, so one decimal
    // place renders a real measurement as "0.0 ms" and reads like a broken
    // counter. Give small numbers the digits they need.
    const shown =
      elapsed >= 10 ? elapsed.toFixed(0) : elapsed >= 1 ? elapsed.toFixed(1) : elapsed.toFixed(3);
    setStatus(
      statements.length +
        (statements.length === 1 ? " statement" : " statements") +
        " in " +
        shown +
        " ms",
      "ok"
    );
  }

  function resetDatabase() {
    const reply = parseJson(api.open(), "open");
    if (!reply.ok) {
      showError(reply.error);
      setStatus("could not open the database", "bad");
      return;
    }
    clearError();
    resultEl.innerHTML = "";
    renderPanel("plan", "Run a query.", true);
    renderPanel("tokens", "Run a query.", true);
    renderPanel("stats", "Run a query.", true);
    renderSchema();
    setStatus("fresh database", "ok");
  }

  // --- wiring ---------------------------------------------------------------

  function buildExamples() {
    const host = $("examples");
    for (const example of EXAMPLES) {
      const button = document.createElement("button");
      button.textContent = example.label;
      button.addEventListener("click", () => {
        sqlBox.value = example.sql;
        sqlBox.focus();
        run();
      });
      host.appendChild(button);
    }
  }

  for (const tab of document.querySelectorAll(".tab")) {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
      document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
      tab.classList.add("active");
      $("panel-" + tab.dataset.panel).classList.add("active");
    });
  }

  runButton.addEventListener("click", run);
  resetButton.addEventListener("click", resetDatabase);
  sqlBox.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
      e.preventDefault();
      run();
    }
  });

  buildExamples();
  runButton.disabled = true;
  resetButton.disabled = true;

  createStrataModule()
    .then((module) => {
      api = bindApi(module);
      const reply = parseJson(api.open(), "open");
      if (!reply.ok) {
        setStatus("engine failed to start", "bad");
        showError(reply.error);
        return;
      }
      runButton.disabled = false;
      resetButton.disabled = false;
      sqlBox.value = EXAMPLES[0].sql;
      renderSchema();
      // Open in a working state. An empty shell with three "Run a query"
      // placeholders shows nothing about what this does.
      run();
    })
    .catch((e) => {
      setStatus("engine failed to load", "bad");
      showError(String(e));
    });
})();
