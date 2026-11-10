// First retrofox smoke test for the SpiderMonkey shell on the phone.
// Run: js.exe smoke.js
const out = [];
function check(name, fn) {
  try {
    out.push(name + ": " + fn());
  } catch (e) {
    out.push(name + ": FAILED " + e);
  }
}

check("engine", () => typeof version === "function" ? version() : "unknown");
check("es2015+", () => [..."abc"].map((c, i) => c + i).join(","));
check("classes", () => { class A { #x = 1; get x() { return this.#x; } } return new A().x; });
check("async", () => typeof (async () => {}));
check("BigInt", () => (2n ** 100n).toString());
check("Intl.NumberFormat", () => new Intl.NumberFormat("de-DE").format(1234567.891));
check("Intl.DateTimeFormat", () => new Intl.DateTimeFormat("fr-FR", { dateStyle: "full", timeZone: "UTC" }).format(new Date(0)));
check("toLocaleUpperCase", () => "istanbul".toLocaleUpperCase("tr"));
check("RegExp unicode", () => /\p{Script=Greek}/u.test("α"));

// Tiny benchmark: fib(25) and a sort of 100k numbers
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
let t = Date.now();
fib(25);
out.push("fib(25): " + (Date.now() - t) + " ms");
t = Date.now();
const a = [];
for (let i = 0; i < 100000; i++) a.push((i * 7919) % 100003);
a.sort((x, y) => x - y);
out.push("sort 100k: " + (Date.now() - t) + " ms");

print(out.join("\n"));
