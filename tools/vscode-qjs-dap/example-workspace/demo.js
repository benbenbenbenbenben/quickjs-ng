function makeCounter(start) {
  let count = start;
  return function increment(step) {
    count += step;
    return count;
  };
}

function main() {
  const increment = makeCounter(1);
  debugger;
  const count = increment(2);
  console.log("count =", count);
}

main();
