function increment(value) {
  const next = value + 1;
  return next;
}

function main() {
  let count = 1;
  debugger;
  count = increment(count);
  console.log("count =", count);
}

main();
