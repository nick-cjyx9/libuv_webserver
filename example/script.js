(async function () {
  btn.onclick = async () => {
    const mj = await fetch("secret/random_number.txt");
    const mjj = await mj.text();
    num.classList.remove("hidden");
    await new Promise((resolve) => setTimeout(resolve, 800));
    num.textContent = mjj;
  };
})();
